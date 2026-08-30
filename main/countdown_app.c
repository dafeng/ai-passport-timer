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
#define BAR_INNER_W       184
#define TICK_MS           200
#define BATTERY_PERIOD_MS 5000
#define CARD_W            68
#define CARD_H            68
#define CARD_GAP_X        8
#define CARD_GAP_Y        10
#define CARD_ORIGIN_X     12
#define CARD_ORIGIN_Y     50

static const char *TAG = "countdown";

static lv_obj_t *make_wrap(lv_obj_t *parent, int x, int y, int w, int h)
{
    /* 包住 panel+阴影，显隐时一起消失。去掉默认主题底，避免再盖住天空。 */
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(wrap, x, y);
    lv_obj_set_size(wrap, w + 6, h + 8);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    return wrap;
}

static countdown_t s_timer;
static lv_obj_t *s_scr;
static lv_obj_t *s_card_wraps[COUNTDOWN_PRESET_COUNT];
static lv_obj_t *s_cards[COUNTDOWN_PRESET_COUNT];
static lv_obj_t *s_card_labels[COUNTDOWN_PRESET_COUNT];
static lv_obj_t *s_time_wrap;
static lv_obj_t *s_bar_wrap;
static lv_obj_t *s_time_panel;
static lv_obj_t *s_time_label;
static lv_obj_t *s_status;
static lv_obj_t *s_bar_fill;
static lv_obj_t *s_hint;
static lv_obj_t *s_battery;
static lv_obj_t *s_mascot;
static lv_timer_t *s_tick;
static TaskHandle_t s_audio_task;
static volatile int s_beep_req;
static bool s_audio_ok;
static bool s_battery_ok;
static uint32_t s_done_flash_ms;

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
        (soc < 20) ? lv_color_hex(UI_RED) : lv_color_hex(UI_PAPER),
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
    }
}

static void refresh_time_widgets(void)
{
    if (!s_time_label || !s_status || !s_bar_fill || !s_hint) {
        return;
    }

    char text[8];
    countdown_format_mmss(s_timer.remaining_ms, text, sizeof(text));
    lv_label_set_text(s_time_label, text);

    uint32_t permille = countdown_remaining_permille(&s_timer);
    int width = (int)((BAR_INNER_W * permille) / 1000u);
    if (width < 0) {
        width = 0;
    }
    if (width < 3 && permille > 0) {
        width = 3;
    }
    lv_obj_set_width(s_bar_fill, width);

    bool urgent = (s_timer.state == COUNTDOWN_STATE_RUNNING ||
                   s_timer.state == COUNTDOWN_STATE_PAUSED) &&
                  s_timer.remaining_ms <= 10000;

    if (s_timer.state == COUNTDOWN_STATE_DONE) {
        uint32_t phase = (s_done_flash_ms / 400u) & 1u;
        lv_obj_set_style_bg_color(
            s_time_panel, lv_color_hex(phase ? UI_RED : UI_YELLOW), 0);
        lv_obj_set_style_text_color(s_time_label, lv_color_hex(UI_INK), 0);
        lv_label_set_text(s_status, "TIME UP");
        lv_label_set_text(s_hint, "OK: AGAIN");
        return;
    }

    if (s_timer.state == COUNTDOWN_STATE_PAUSED) {
        lv_obj_set_style_bg_color(s_time_panel, lv_color_hex(UI_ORANGE), 0);
        lv_obj_set_style_text_color(
            s_time_label, lv_color_hex(urgent ? UI_RED : UI_INK), 0);
        lv_label_set_text(s_status, "PAUSED");
        lv_label_set_text(s_hint, "OK:RESUME  HOLD:BACK");
        return;
    }

    lv_obj_set_style_bg_color(s_time_panel, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_text_color(
        s_time_label, lv_color_hex(urgent ? UI_RED : UI_INK), 0);
    lv_label_set_text(s_status, "RUNNING");
    lv_label_set_text(s_hint, "OK:PAUSE  HOLD:BACK");
}

static void refresh_ui(void)
{
    bool selecting = (s_timer.state == COUNTDOWN_STATE_SELECT);
    for (int i = 0; i < COUNTDOWN_PRESET_COUNT; i++) {
        set_hidden(s_card_wraps[i], !selecting);
    }
    set_hidden(s_time_wrap, selecting);
    set_hidden(s_bar_wrap, selecting);

    if (selecting) {
        refresh_cards();
        if (s_hint) {
            lv_label_set_text(s_hint, "UP/DOWN   OK START");
        }
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

    bool just_done = countdown_tick(&s_timer, now_ms());
    if (s_timer.state == COUNTDOWN_STATE_DONE) {
        s_done_flash_ms += TICK_MS;
    }
    if (just_done) {
        s_done_flash_ms = 0;
        s_beep_req = 1;
        ui_pixel_mascot_jump(s_mascot);
    }
    if (s_timer.state != COUNTDOWN_STATE_SELECT) {
        refresh_time_widgets();
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

    /* 先建主题屏并立刻载入，避免后面控件创建失败时整屏空白。 */
    s_scr = ui_pixel_screen_create("TIMER");
    lv_screen_load(s_scr);

    s_battery = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(s_battery, 192, 32);

    /* 3x2 大方块：5m 10m 15m / 20m 25m 30m，选中项放大字号。 */
    for (int i = 0; i < COUNTDOWN_PRESET_COUNT; i++) {
        int col = i % 3;
        int row = i / 3;
        int x = CARD_ORIGIN_X + col * (CARD_W + CARD_GAP_X);
        int y = CARD_ORIGIN_Y + row * (CARD_H + CARD_GAP_Y);
        s_card_wraps[i] = make_wrap(s_scr, x, y, CARD_W, CARD_H);
        s_cards[i] = ui_pixel_panel_create(s_card_wraps[i], 0, 0, CARD_W, CARD_H,
                                          UI_PAPER);
        lv_obj_set_style_pad_all(s_cards[i], 0, 0);
        s_card_labels[i] = ui_pixel_label(s_cards[i], COUNTDOWN_PRESET_LABELS[i],
                                          &lv_font_montserrat_20, UI_INK);
        lv_obj_center(s_card_labels[i]);
    }

    s_time_wrap = make_wrap(s_scr, 16, 50, 208, 118);
    s_time_panel = ui_pixel_panel_create(s_time_wrap, 0, 0, 208, 118, UI_YELLOW);
    s_time_label = ui_pixel_label(s_time_panel, "05:00",
                                  &lv_font_montserrat_20, UI_INK);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 22);

    s_status = ui_pixel_label(s_time_panel, "RUNNING",
                              &lv_font_montserrat_14, UI_INK);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -12);

    s_bar_wrap = make_wrap(s_scr, 16, 176, 208, 26);
    lv_obj_t *bar = ui_pixel_panel_create(s_bar_wrap, 0, 0, 208, 26, UI_INK);
    s_bar_fill = lv_obj_create(bar);
    lv_obj_remove_flag(s_bar_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_bar_fill, BAR_INNER_W, 10);
    lv_obj_set_style_radius(s_bar_fill, 0, 0);
    lv_obj_set_style_border_width(s_bar_fill, 0, 0);
    lv_obj_set_style_pad_all(s_bar_fill, 0, 0);
    lv_obj_set_style_bg_color(s_bar_fill, lv_color_hex(UI_GRASS), 0);
    lv_obj_align(s_bar_fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_hint = ui_pixel_label(s_scr, "UP/DOWN   OK START",
                            &lv_font_montserrat_14, UI_PAPER);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 208);

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 238);

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
        memset(s_card_wraps, 0, sizeof(s_card_wraps));
        memset(s_cards, 0, sizeof(s_cards));
        memset(s_card_labels, 0, sizeof(s_card_labels));
        s_time_wrap = s_bar_wrap = NULL;
        s_time_panel = s_time_label = s_status = s_bar_fill = NULL;
        s_hint = s_battery = s_mascot = NULL;
    }
}

void countdown_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    uint32_t t = now_ms();

    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        if (s_timer.state != COUNTDOWN_STATE_SELECT) {
            countdown_reset(&s_timer);
            refresh_ui();
            ui_pixel_mascot_jump(s_mascot);
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
            ui_pixel_mascot_jump(s_mascot);
        } else if (btn == BSP_BTN_DOWN) {
            countdown_select_next(&s_timer);
            refresh_ui();
            ui_pixel_mascot_jump(s_mascot);
        } else if (btn == BSP_BTN_OK) {
            countdown_start(&s_timer, t);
            refresh_ui();
            ui_pixel_mascot_jump(s_mascot);
        }
        return;
    }

    if (s_timer.state == COUNTDOWN_STATE_DONE) {
        if (btn == BSP_BTN_OK) {
            countdown_reset(&s_timer);
            refresh_ui();
            ui_pixel_mascot_jump(s_mascot);
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
    ui_pixel_mascot_jump(s_mascot);
}

void countdown_app_set_peripherals(bool audio_ok, bool battery_ok)
{
    s_audio_ok = audio_ok;
    s_battery_ok = battery_ok;
}
