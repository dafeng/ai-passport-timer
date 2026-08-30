#include "countdown_app.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "countdown_timer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui_pixel.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE       16000
#define TONE_HZ           1000
#define CHUNK_SAMPLES     256
#define BEEP_MS           140
#define BEEP_GAP_MS       90
#define BEEP_COUNT        3
#define TICK_MS           80
#define BATTERY_PERIOD_MS 5000
#define CX                120
#define CY                148
#define RING_STEPS        36
#define PIP_SIZE          12
#define TICK_SIZE         8
#define TICK_SEL          14

static const char *TAG = "countdown";

/* 半径 92 的圆环采样点。0° 在正右，起步 -90° 在正上，顺时针。C3 无 FPU，不用运行时浮点。 */
static const int16_t RING_XY[RING_STEPS][2] = {
    {0, -92}, {16, -91}, {31, -86}, {46, -80}, {59, -70}, {70, -59},
    {80, -46}, {86, -31}, {91, -16}, {92, 0}, {91, 16}, {86, 31},
    {80, 46}, {70, 59}, {59, 70}, {46, 80}, {31, 86}, {16, 91},
    {0, 92}, {-16, 91}, {-31, 86}, {-46, 80}, {-59, 70}, {-70, 59},
    {-80, 46}, {-86, 31}, {-91, 16}, {-92, 0}, {-91, -16}, {-86, -31},
    {-80, -46}, {-70, -59}, {-59, -70}, {-46, -80}, {-31, -86}, {-16, -91},
};

static lv_obj_t *px(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *circle(lv_obj_t *parent, int x, int y, int d, uint32_t color)
{
    lv_obj_t *obj = px(parent, x, y, d, d, color);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    return obj;
}

static void place_on_ring(lv_obj_t *obj, int step, int size)
{
    int x = CX + RING_XY[step][0] - size / 2;
    int y = CY + RING_XY[step][1] - size / 2;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, size, size);
}

static void place_pip(uint32_t permille)
{
    uint32_t elapsed = 1000u - permille;
    uint32_t scaled = elapsed * RING_STEPS;
    uint32_t i0 = scaled / 1000u;
    uint32_t frac = scaled % 1000u;
    if (i0 >= RING_STEPS) {
        i0 = 0;
        frac = 0;
    }
    uint32_t i1 = i0 + 1u;
    if (i1 >= RING_STEPS) {
        i1 = 0;
    }
    int x0 = RING_XY[i0][0];
    int y0 = RING_XY[i0][1];
    int dx = RING_XY[i1][0] - x0;
    int dy = RING_XY[i1][1] - y0;
    int x = CX + x0 + dx * (int)frac / 1000 - PIP_SIZE / 2;
    int y = CY + y0 + dy * (int)frac / 1000 - PIP_SIZE / 2;
    lv_obj_set_pos(s_pip, x, y);
    lv_obj_set_size(s_pip, PIP_SIZE, PIP_SIZE);
}

static void layout_labels(void)
{
    if (s_clock) {
        lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, CY - 30);
    }
    if (s_sub) {
        lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, CY + 26);
    }
    if (s_hint) {
        lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -12);
    }
}

static countdown_t s_timer;
static lv_obj_t *s_scr;
static lv_obj_t *s_glow;
static lv_obj_t *s_orb;
static lv_obj_t *s_ring;
static lv_obj_t *s_ticks[COUNTDOWN_PRESET_COUNT];
static lv_obj_t *s_pip;
static lv_obj_t *s_clock;
static lv_obj_t *s_sub;
static lv_obj_t *s_hint;
static lv_obj_t *s_battery;
static lv_timer_t *s_tick;
static TaskHandle_t s_audio_task;
static volatile int s_beep_req;
static bool s_audio_ok;
static bool s_battery_ok;
static uint32_t s_done_flash_ms;
static uint32_t s_fx_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_battery(void)
{
    if (!s_battery) {
        return;
    }
    if (!s_battery_ok) {
        lv_label_set_text(s_battery, "");
        return;
    }
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_battery, "");
        return;
    }
    lv_label_set_text_fmt(s_battery, "%d%%", soc);
    lv_obj_set_style_text_color(
        s_battery,
        (soc < 20) ? lv_color_hex(UI_RED) : lv_color_hex(UI_MUTED),
        0);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -12, 10);
}

static void paint_ring(uint32_t color)
{
    if (s_ring) {
        lv_obj_set_style_border_color(s_ring, lv_color_hex(color), 0);
    }
    if (s_pip) {
        lv_obj_set_style_bg_color(s_pip, lv_color_hex(color), 0);
    }
}

static void set_glow(uint32_t color, bool pulse)
{
    if (!s_glow) {
        return;
    }
    if (pulse) {
        uint32_t t = (s_fx_ms / 50u) % 40u;
        int opa = (t < 20u) ? (10 + (int)t) : (10 + (40 - (int)t));
        lv_obj_set_style_bg_color(s_glow, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(s_glow, opa, 0);
        return;
    }
    lv_obj_set_style_bg_color(s_glow, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(s_glow, LV_OPA_10, 0);
}

static void refresh_select(void)
{
    char clock[8];
    countdown_format_mmss(COUNTDOWN_PRESET_MS[s_timer.preset_index],
                          clock, sizeof(clock));
    lv_label_set_text(s_clock, clock);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(UI_PAPER), 0);
    lv_label_set_text_fmt(s_sub, "%d MIN",
                          (int)(COUNTDOWN_PRESET_MS[s_timer.preset_index] / 60000u));
    lv_obj_set_style_text_color(s_sub, lv_color_hex(UI_CYAN), 0);

    for (int i = 0; i < COUNTDOWN_PRESET_COUNT; i++) {
        bool sel = (i == s_timer.preset_index);
        int size = sel ? TICK_SEL : TICK_SIZE;
        place_on_ring(s_ticks[i], i * (RING_STEPS / COUNTDOWN_PRESET_COUNT), size);
        lv_obj_set_style_bg_color(
            s_ticks[i],
            lv_color_hex(sel ? UI_CYAN : UI_LINE),
            0);
        set_hidden(s_ticks[i], false);
    }
    set_hidden(s_pip, true);
    paint_ring(UI_CYAN);
    set_glow(UI_CYAN, false);
    layout_labels();
    if (s_hint) {
        lv_label_set_text(s_hint, "UP / DOWN      OK");
        lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -12);
    }
}

static void refresh_run(void)
{
    char text[8];
    countdown_format_mmss(s_timer.remaining_ms, text, sizeof(text));

    bool urgent = (s_timer.state == COUNTDOWN_STATE_RUNNING ||
                   s_timer.state == COUNTDOWN_STATE_PAUSED) &&
                  s_timer.remaining_ms <= 10000;
    bool blink = ((s_fx_ms / 400u) & 1u) == 0;
    uint32_t accent = UI_CYAN;

    if (s_timer.state == COUNTDOWN_STATE_DONE) {
        accent = blink ? UI_RED : UI_YELLOW;
        lv_label_set_text(s_sub, "TIME UP");
        if (s_hint) {
            lv_label_set_text(s_hint, "OK  again");
        }
    } else if (s_timer.state == COUNTDOWN_STATE_PAUSED) {
        accent = urgent ? UI_RED : UI_ORANGE;
        lv_label_set_text(s_sub, "PAUSED");
        if (s_hint) {
            lv_label_set_text(s_hint, "OK resume    hold back");
        }
    } else if (urgent) {
        accent = UI_RED;
        lv_label_set_text(s_sub, "LAST 10s");
        if (s_hint) {
            lv_label_set_text(s_hint, "OK pause     hold back");
        }
    } else {
        lv_label_set_text(s_sub, "RUNNING");
        if (s_hint) {
            lv_label_set_text(s_hint, "OK pause     hold back");
        }
        if (s_timer.state == COUNTDOWN_STATE_RUNNING && !blink) {
            text[2] = ' ';
        }
    }

    lv_label_set_text(s_clock, text);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(accent == UI_CYAN ? UI_PAPER : accent), 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(accent), 0);

    for (int i = 0; i < COUNTDOWN_PRESET_COUNT; i++) {
        set_hidden(s_ticks[i], true);
    }

    set_hidden(s_pip, false);
    place_pip(countdown_remaining_permille(&s_timer));
    paint_ring(accent);
    set_glow(accent, s_timer.state == COUNTDOWN_STATE_DONE);
    layout_labels();
}

static void refresh_ui(void)
{
    if (!s_clock || !s_sub) {
        return;
    }
    if (s_timer.state == COUNTDOWN_STATE_SELECT) {
        refresh_select();
    } else {
        refresh_run();
    }
}

static void build_face(void)
{
    s_glow = circle(s_scr, CX - 110, CY - 110, 220, UI_CYAN);
    lv_obj_set_style_bg_opa(s_glow, LV_OPA_10, 0);

    s_orb = circle(s_scr, CX - 78, CY - 78, 156, UI_PANEL);

    s_ring = circle(s_scr, CX - 92, CY - 92, 184, UI_BG);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring, 3, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(UI_CYAN), 0);

    for (int i = 0; i < COUNTDOWN_PRESET_COUNT; i++) {
        s_ticks[i] = circle(s_scr, 0, 0, TICK_SIZE, UI_LINE);
    }

    s_pip = circle(s_scr, 0, 0, PIP_SIZE, UI_CYAN);
    set_hidden(s_pip, true);

    s_clock = ui_pixel_label(s_scr, "05:00", &lv_font_montserrat_48, UI_PAPER);
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, CY - 30);

    s_sub = ui_pixel_label(s_scr, "5 MIN", &lv_font_montserrat_14, UI_CYAN);
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, CY + 26);
}

static void play_beeps(void)
{
    if (!s_audio_ok) {
        return;
    }
    if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
        return;
    }
    bsp_audio_set_volume(80);

    int16_t *buf = malloc(CHUNK_SAMPLES * sizeof(int16_t));
    if (!buf) {
        return;
    }

    const int period = SAMPLE_RATE / TONE_HZ;
    const int tone_samples = SAMPLE_RATE * BEEP_MS / 1000;
    const int gap_samples = SAMPLE_RATE * BEEP_GAP_MS / 1000;

    for (int beep = 0; beep < BEEP_COUNT && s_scr != NULL; beep++) {
        int phase = 0;
        int left = tone_samples;
        while (left > 0) {
            int n = left < CHUNK_SAMPLES ? left : CHUNK_SAMPLES;
            for (int i = 0; i < n; i++) {
                buf[i] = (phase < period / 2) ? 7000 : -7000;
                if (++phase >= period) {
                    phase = 0;
                }
            }
            bsp_audio_write(buf, (size_t)n * sizeof(int16_t));
            left -= n;
        }

        memset(buf, 0, CHUNK_SAMPLES * sizeof(int16_t));
        left = gap_samples;
        while (left > 0) {
            int n = left < CHUNK_SAMPLES ? left : CHUNK_SAMPLES;
            bsp_audio_write(buf, (size_t)n * sizeof(int16_t));
            left -= n;
        }
    }
    free(buf);
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_beep_req) {
            s_beep_req = 0;
            play_beeps();
        } else {
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
}

static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    static uint32_t battery_acc;

    s_fx_ms += TICK_MS;
    bool just_done = countdown_tick(&s_timer, now_ms());
    if (s_timer.state == COUNTDOWN_STATE_DONE) {
        s_done_flash_ms += TICK_MS;
    }
    if (just_done) {
        s_done_flash_ms = 0;
        s_beep_req = 1;
    }

    if (s_timer.state != COUNTDOWN_STATE_SELECT) {
        refresh_ui();
    }

    battery_acc += TICK_MS;
    if (battery_acc >= BATTERY_PERIOD_MS) {
        battery_acc = 0;
        refresh_battery();
    }
}

void countdown_app_enter(void)
{
    countdown_init(&s_timer);
    s_beep_req = 0;
    s_done_flash_ms = 0;
    s_fx_ms = 0;

    s_scr = ui_pixel_screen_create("CHRONO");
    lv_screen_load(s_scr);

    s_battery = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -12, 10);

    s_hint = ui_pixel_label(s_scr, "UP / DOWN      OK",
                            &lv_font_montserrat_14, UI_MUTED);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    build_face();
    refresh_battery();
    refresh_ui();

    s_tick = lv_timer_create(on_tick, TICK_MS, NULL);
    if (!s_audio_task && s_audio_ok) {
        xTaskCreate(audio_task, "timer_beep", 3072, NULL, 4, &s_audio_task);
    }
    ESP_LOGI(TAG, "UI ready");
}

void countdown_app_exit(void)
{
    s_beep_req = 0;
    if (s_audio_task) {
        vTaskDelete(s_audio_task);
        s_audio_task = NULL;
    }
    if (s_tick) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_glow = s_orb = s_ring = s_pip = s_clock = s_sub = NULL;
        s_hint = s_battery = NULL;
        memset(s_ticks, 0, sizeof(s_ticks));
    }
}

void countdown_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    uint32_t t = now_ms();

    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        if (s_timer.state != COUNTDOWN_STATE_SELECT) {
            countdown_reset(&s_timer);
            refresh_ui();
        }
        return;
    }

    if (ev != BSP_BTN_CLICK) {
        return;
    }

    if (s_timer.state == COUNTDOWN_STATE_SELECT) {
        if (btn == BSP_BTN_UP) {
            countdown_select_prev(&s_timer);
            refresh_ui();
        } else if (btn == BSP_BTN_DOWN) {
            countdown_select_next(&s_timer);
            refresh_ui();
        } else if (btn == BSP_BTN_OK) {
            countdown_start(&s_timer, t);
            refresh_ui();
        }
        return;
    }

    if (s_timer.state == COUNTDOWN_STATE_DONE) {
        if (btn == BSP_BTN_OK) {
            countdown_reset(&s_timer);
            refresh_ui();
        }
        return;
    }

    if (btn != BSP_BTN_OK) {
        return;
    }
    if (s_timer.state == COUNTDOWN_STATE_RUNNING) {
        countdown_pause(&s_timer, t);
    } else if (s_timer.state == COUNTDOWN_STATE_PAUSED) {
        countdown_resume(&s_timer, t);
    }
    refresh_ui();
}

void countdown_app_set_peripherals(bool audio_ok, bool battery_ok)
{
    s_audio_ok = audio_ok;
    s_battery_ok = battery_ok;
}
