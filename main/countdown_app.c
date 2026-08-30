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
#define CARD_W            68
#define CARD_H            70
#define SEG_COUNT         8
#define TILE_W            42
#define TILE_H            72
#define SEG_OFF           0x1A2838

static const char *TAG = "countdown";

typedef enum {
    UI_MODE_NONE = 0,
    UI_MODE_SELECT,
    UI_MODE_RUN,
} ui_mode_t;

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

static lv_obj_t *make_layer(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(layer, 0, y);
    lv_obj_set_size(layer, 240, h);
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    return layer;
}

static countdown_t s_timer;
static ui_mode_t s_mode;
static lv_obj_t *s_scr;
static lv_obj_t *s_layer;
static lv_obj_t *s_cards[COUNTDOWN_PRESET_COUNT];
static lv_obj_t *s_card_labels[COUNTDOWN_PRESET_COUNT];
static lv_obj_t *s_preview;
static lv_obj_t *s_tiles[4];
static lv_obj_t *s_tile_labels[4];
static lv_obj_t *s_colon_top;
static lv_obj_t *s_colon_bot;
static lv_obj_t *s_segs[SEG_COUNT];
static lv_obj_t *s_led;
static lv_obj_t *s_status;
static lv_obj_t *s_scan;
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
        (soc < 20) ? lv_color_hex(UI_RED) : lv_color_hex(UI_CYAN),
        0);
}

static void refresh_cards(void)
{
    for (int i = 0; i < COUNTDOWN_PRESET_COUNT; i++) {
        if (!s_cards[i]) {
            continue;
        }
        bool sel = (i == s_timer.preset_index);
        ui_pixel_set_selected(s_cards[i], sel, true);
        lv_obj_set_style_text_font(
            s_card_labels[i],
            sel ? &lv_font_montserrat_20 : &lv_font_montserrat_14,
            0);
        lv_obj_set_style_text_color(
            s_card_labels[i],
            lv_color_hex(sel ? UI_CYAN : UI_PAPER),
            0);
    }
    if (s_preview) {
        char clock[8];
        countdown_format_mmss(COUNTDOWN_PRESET_MS[s_timer.preset_index],
                              clock, sizeof(clock));
        lv_label_set_text_fmt(s_preview, "T-MINUS  %s", clock);
    }
}

static void set_clock_text(const char *mmss)
{
    const int map[4] = {0, 1, 3, 4};
    char buf[2] = {0};
    for (int i = 0; i < 4; i++) {
        if (!s_tile_labels[i]) {
            return;
        }
        buf[0] = mmss[map[i]];
        lv_label_set_text(s_tile_labels[i], buf);
    }
}

static void refresh_meter(uint32_t permille, uint32_t on_color)
{
    uint32_t lit = (permille * SEG_COUNT + 999u) / 1000u;
    if (permille > 0 && lit == 0) {
        lit = 1;
    }
    for (int i = 0; i < SEG_COUNT; i++) {
        if (!s_segs[i]) {
            continue;
        }
        lv_obj_set_style_bg_color(
            s_segs[i],
            lv_color_hex((unsigned)i < lit ? on_color : SEG_OFF),
            0);
    }
}

static void refresh_time_widgets(void)
{
    if (!s_tile_labels[0] || !s_status || !s_hint) {
        return;
    }

    char text[8];
    countdown_format_mmss(s_timer.remaining_ms, text, sizeof(text));
    set_clock_text(text);

    uint32_t permille = countdown_remaining_permille(&s_timer);
    bool urgent = (s_timer.state == COUNTDOWN_STATE_RUNNING ||
                   s_timer.state == COUNTDOWN_STATE_PAUSED) &&
                  s_timer.remaining_ms <= 10000;
    bool blink = ((s_fx_ms / 400u) & 1u) == 0;

    uint32_t digit = UI_CYAN;
    uint32_t tile = UI_PANEL;
    uint32_t meter = UI_CYAN;
    uint32_t led = UI_CYAN;

    if (s_timer.state == COUNTDOWN_STATE_DONE) {
        digit = blink ? UI_BG : UI_PAPER;
        tile = blink ? UI_RED : UI_YELLOW;
        meter = UI_RED;
        led = blink ? UI_RED : UI_YELLOW;
        lv_label_set_text(s_status, "LOCK  TIME-UP");
        lv_label_set_text(s_hint, "SIGNAL  OK AGAIN");
    } else if (s_timer.state == COUNTDOWN_STATE_PAUSED) {
        digit = UI_ORANGE;
        meter = urgent ? UI_RED : UI_ORANGE;
        led = blink ? UI_ORANGE : SEG_OFF;
        lv_label_set_text(s_status, "HOLD  STANDBY");
        lv_label_set_text(s_hint, "OK RESUME   HOLD BACK");
    } else {
        if (urgent) {
            digit = UI_RED;
            tile = UI_PANEL_HI;
            meter = UI_RED;
            led = blink ? UI_RED : SEG_OFF;
            lv_label_set_text(s_status, "WARN  FINAL-10");
        } else {
            led = blink ? UI_CYAN : SEG_OFF;
            lv_label_set_text(s_status, "LIVE  COUNTDOWN");
        }
        lv_label_set_text(s_hint, "OK PAUSE    HOLD BACK");
    }

    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_tiles[i], lv_color_hex(tile), 0);
        lv_obj_set_style_text_color(s_tile_labels[i], lv_color_hex(digit), 0);
    }
    refresh_meter(permille, meter);
    if (s_led) {
        lv_obj_set_style_bg_color(s_led, lv_color_hex(led), 0);
    }

    bool colon_on = (s_timer.state != COUNTDOWN_STATE_RUNNING) ||
                    ((s_fx_ms / 500u) & 1u) == 0;
    if (s_colon_top && s_colon_bot) {
        lv_obj_set_style_bg_opa(s_colon_top, colon_on ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_bg_opa(s_colon_bot, colon_on ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

static void clear_layer(void)
{
    if (s_layer) {
        lv_obj_delete(s_layer);
        s_layer = NULL;
    }
    memset(s_cards, 0, sizeof(s_cards));
    memset(s_card_labels, 0, sizeof(s_card_labels));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_tile_labels, 0, sizeof(s_tile_labels));
    memset(s_segs, 0, sizeof(s_segs));
    s_preview = s_colon_top = s_colon_bot = s_led = s_status = NULL;
    s_mode = UI_MODE_NONE;
}

static void build_select(void)
{
    clear_layer();
    s_layer = make_layer(s_scr, 42, 236);

    s_preview = ui_pixel_label(s_layer, "T-MINUS  05:00",
                               &lv_font_montserrat_14, UI_CYAN);
    lv_obj_align(s_preview, LV_ALIGN_TOP_MID, 0, 12);

    px(s_layer, 28, 36, 184, 2, UI_LINE);

    for (int i = 0; i < COUNTDOWN_PRESET_COUNT; i++) {
        int col = i % 3;
        int row = i / 3;
        int x = 14 + col * (CARD_W + 8);
        int y = 52 + row * (CARD_H + 14);
        s_cards[i] = ui_pixel_panel_create(s_layer, x, y, CARD_W, CARD_H, UI_PANEL);
        lv_obj_set_style_pad_all(s_cards[i], 0, 0);
        s_card_labels[i] = ui_pixel_label(s_cards[i], COUNTDOWN_PRESET_LABELS[i],
                                          &lv_font_montserrat_20, UI_PAPER);
        lv_obj_center(s_card_labels[i]);
    }

    s_mode = UI_MODE_SELECT;
    refresh_cards();
    if (s_hint) {
        lv_label_set_text(s_hint, "UP/DOWN   OK ARM");
    }
}

static void build_run(void)
{
    clear_layer();
    s_layer = make_layer(s_scr, 42, 236);

    s_led = px(s_layer, 16, 16, 10, 10, UI_CYAN);
    lv_obj_set_style_radius(s_led, LV_RADIUS_CIRCLE, 0);
    s_status = ui_pixel_label(s_layer, "LIVE  COUNTDOWN",
                              &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_pos(s_status, 34, 14);

    const int clock_x = 16;
    const int clock_y = 44;
    const int gap = 6;
    for (int i = 0; i < 4; i++) {
        int x = clock_x + i * (TILE_W + gap);
        if (i >= 2) {
            x += 12;
        }
        s_tiles[i] = ui_pixel_panel_create(s_layer, x, clock_y, TILE_W, TILE_H,
                                           UI_PANEL);
        lv_obj_set_style_pad_all(s_tiles[i], 0, 0);
        s_tile_labels[i] = ui_pixel_label(s_tiles[i], "0",
                                          &lv_font_montserrat_20, UI_CYAN);
        lv_obj_center(s_tile_labels[i]);
    }
    int colon_x = clock_x + 2 * (TILE_W + gap) + 2;
    s_colon_top = px(s_layer, colon_x, 64, 6, 6, UI_CYAN);
    s_colon_bot = px(s_layer, colon_x, 90, 6, 6, UI_CYAN);

    for (int i = 0; i < SEG_COUNT; i++) {
        s_segs[i] = px(s_layer, 16 + i * 27, 136, 23, 10, UI_CYAN);
        lv_obj_set_style_radius(s_segs[i], 2, 0);
    }

    s_mode = UI_MODE_RUN;
    refresh_time_widgets();
}

static void sync_mode(void)
{
    bool selecting = (s_timer.state == COUNTDOWN_STATE_SELECT);
    if (selecting && s_mode != UI_MODE_SELECT) {
        build_select();
    } else if (!selecting && s_mode != UI_MODE_RUN) {
        build_run();
    } else if (selecting) {
        refresh_cards();
    } else {
        refresh_time_widgets();
    }
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

    if (s_scan) {
        int y = 42 + (int)((s_fx_ms / 16u) % 236u);
        lv_obj_set_y(s_scan, y);
        lv_obj_move_foreground(s_scan);
    }

    sync_mode();

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
    s_mode = UI_MODE_NONE;

    s_scr = ui_pixel_screen_create("CHRONO");
    lv_screen_load(s_scr);

    s_scan = px(s_scr, 0, 42, 240, 2, UI_CYAN);
    lv_obj_set_style_bg_opa(s_scan, LV_OPA_20, 0);

    s_battery = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_CYAN);
    lv_obj_set_pos(s_battery, 188, 8);

    s_hint = ui_pixel_label(s_scr, "UP/DOWN   OK ARM",
                            &lv_font_montserrat_14, UI_MUTED);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    refresh_battery();
    build_select();

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
        s_layer = NULL;
        memset(s_cards, 0, sizeof(s_cards));
        memset(s_card_labels, 0, sizeof(s_card_labels));
        memset(s_tiles, 0, sizeof(s_tiles));
        memset(s_tile_labels, 0, sizeof(s_tile_labels));
        memset(s_segs, 0, sizeof(s_segs));
        s_preview = s_colon_top = s_colon_bot = s_led = s_status = NULL;
        s_scan = s_hint = s_battery = NULL;
        s_mode = UI_MODE_NONE;
    }
}

void countdown_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    uint32_t t = now_ms();

    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        if (s_timer.state != COUNTDOWN_STATE_SELECT) {
            countdown_reset(&s_timer);
            sync_mode();
        }
        return;
    }

    if (ev != BSP_BTN_CLICK) {
        return;
    }

    if (s_timer.state == COUNTDOWN_STATE_SELECT) {
        if (btn == BSP_BTN_UP) {
            countdown_select_prev(&s_timer);
            refresh_cards();
        } else if (btn == BSP_BTN_DOWN) {
            countdown_select_next(&s_timer);
            refresh_cards();
        } else if (btn == BSP_BTN_OK) {
            countdown_start(&s_timer, t);
            sync_mode();
        }
        return;
    }

    if (s_timer.state == COUNTDOWN_STATE_DONE) {
        if (btn == BSP_BTN_OK) {
            countdown_reset(&s_timer);
            sync_mode();
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
    refresh_time_widgets();
}

void countdown_app_set_peripherals(bool audio_ok, bool battery_ok)
{
    s_audio_ok = audio_ok;
    s_battery_ok = battery_ok;
}
