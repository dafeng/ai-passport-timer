#include "countdown_app.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "countdown_timer.h"
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
#define BAR_INNER_W       188
#define TICK_MS           100
#define BATTERY_PERIOD_MS 5000
#define TILE_W            40
#define TILE_H            56

static const char *const PRESET_NUM[COUNTDOWN_PRESET_COUNT] = {
    "30", "5", "10", "15", "20", "30",
};
static const char *const PRESET_UNIT[COUNTDOWN_PRESET_COUNT] = {
    "SEC", "MIN", "MIN", "MIN", "MIN", "MIN",
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

static lv_obj_t *make_group(lv_obj_t *parent)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(group, 0, 0);
    lv_obj_set_size(group, 240, 210);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(group, 0, 0);
    lv_obj_set_style_outline_width(group, 0, 0);
    lv_obj_set_style_radius(group, 0, 0);
    lv_obj_set_style_pad_all(group, 0, 0);
    return group;
}

static countdown_t s_timer;
static lv_obj_t *s_scr;
static lv_obj_t *s_select_group;
static lv_obj_t *s_run_group;
static lv_obj_t *s_cards[COUNTDOWN_PRESET_COUNT];
static lv_obj_t *s_card_nums[COUNTDOWN_PRESET_COUNT];
static lv_obj_t *s_card_units[COUNTDOWN_PRESET_COUNT];
static lv_obj_t *s_card_tabs[COUNTDOWN_PRESET_COUNT];
static lv_obj_t *s_time_panel;
static lv_obj_t *s_tiles[4];
static lv_obj_t *s_tile_labels[4];
static lv_obj_t *s_colon_top;
static lv_obj_t *s_colon_bot;
static lv_obj_t *s_preset_caption;
static lv_obj_t *s_bar_track;
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

static void set_group_visible(lv_obj_t *group, bool visible)
{
    if (!group) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(group, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(group, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 白云在 (188,8)，电量放在其下方空闲蓝天区。soc=-1 时不画数字。 */
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
    lv_label_set_text_fmt(s_battery, "%d", soc);
    lv_obj_set_style_text_color(
        s_battery,
        (soc < 20) ? lv_color_hex(UI_RED) : lv_color_hex(UI_INK),
        0);
}

static void refresh_cards(void)
{
    for (int i = 0; i < COUNTDOWN_PRESET_COUNT; i++) {
        bool sel = (i == s_timer.preset_index);
        ui_pixel_set_selected(s_cards[i], sel, true);
        lv_obj_set_style_text_font(
            s_card_nums[i],
            sel ? &lv_font_montserrat_20 : &lv_font_montserrat_14,
            0);
        if (sel) {
            lv_obj_remove_flag(s_card_tabs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_card_tabs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void set_tiles_palette(uint32_t tile_bg, uint32_t digit, uint32_t colon)
{
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_tiles[i], lv_color_hex(tile_bg), 0);
        lv_obj_set_style_text_color(s_tile_labels[i], lv_color_hex(digit), 0);
    }
    lv_obj_set_style_bg_color(s_colon_top, lv_color_hex(colon), 0);
    lv_obj_set_style_bg_color(s_colon_bot, lv_color_hex(colon), 0);
}

static void set_clock_text(const char *mmss)
{
    /* mmss 形如 "MM:SS"，取四个数字位。 */
    const int map[4] = {0, 1, 3, 4};
    char buf[2] = {0};
    for (int i = 0; i < 4; i++) {
        buf[0] = mmss[map[i]];
        lv_label_set_text(s_tile_labels[i], buf);
    }
}

static void refresh_time_widgets(void)
{
    char text[8];
    countdown_format_mmss(s_timer.remaining_ms, text, sizeof(text));
    set_clock_text(text);

    uint32_t permille = countdown_remaining_permille(&s_timer);
    int width = (int)((BAR_INNER_W * permille) / 1000u);
    if (width < 0) {
        width = 0;
    }
    if (width < 4 && permille > 0) {
        width = 4;
    }
    lv_obj_set_width(s_bar_fill, width);

    int idx = s_timer.preset_index;
    lv_label_set_text_fmt(s_preset_caption, "%s %s", PRESET_NUM[idx], PRESET_UNIT[idx]);

    bool urgent = (s_timer.state == COUNTDOWN_STATE_RUNNING ||
                   s_timer.state == COUNTDOWN_STATE_PAUSED) &&
                  s_timer.remaining_ms <= 10000;

    if (s_timer.state == COUNTDOWN_STATE_DONE) {
        uint32_t phase = (s_done_flash_ms / 350u) & 1u;
        uint32_t panel = phase ? UI_RED : UI_YELLOW;
        lv_obj_set_style_bg_color(s_time_panel, lv_color_hex(panel), 0);
        set_tiles_palette(phase ? UI_YELLOW : UI_PAPER, UI_INK, UI_INK);
        lv_obj_set_style_bg_color(s_bar_fill, lv_color_hex(UI_RED), 0);
        lv_label_set_text(s_hint, "TIME UP   OK: AGAIN");
        lv_obj_remove_flag(s_colon_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_colon_bot, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (s_timer.state == COUNTDOWN_STATE_PAUSED) {
        lv_obj_set_style_bg_color(s_time_panel, lv_color_hex(0xFFF3D6), 0);
        set_tiles_palette(UI_ORANGE, UI_INK, UI_INK);
        lv_obj_set_style_bg_color(
            s_bar_fill, lv_color_hex(urgent ? UI_RED : UI_ORANGE), 0);
        lv_label_set_text(s_hint, "OK RESUME    HOLD BACK");
        lv_obj_remove_flag(s_colon_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_colon_bot, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_set_style_bg_color(s_time_panel, lv_color_hex(UI_PAPER), 0);
    uint32_t digit = urgent ? UI_RED : UI_INK;
    uint32_t tile = urgent ? UI_YELLOW : UI_PAPER;
    set_tiles_palette(tile, digit, urgent ? UI_RED : UI_INK);
    lv_obj_set_style_bg_color(
        s_bar_fill, lv_color_hex(urgent ? UI_RED : UI_GRASS), 0);
    lv_label_set_text(s_hint, "OK PAUSE     HOLD BACK");

    /* 运行中冒号闪烁，一秒一眼，避免数字像冻结。 */
    bool colon_on = ((now_ms() / 500u) & 1u) == 0;
    if (colon_on) {
        lv_obj_remove_flag(s_colon_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_colon_bot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_colon_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_colon_bot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_ui(void)
{
    bool selecting = (s_timer.state == COUNTDOWN_STATE_SELECT);
    set_group_visible(s_select_group, selecting);
    set_group_visible(s_run_group, !selecting);
    if (selecting) {
        refresh_cards();
        lv_label_set_text(s_hint, "UP / DOWN     OK START");
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

/* lv_timer 已在 LVGL 任务持锁上下文中运行。 */
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

static lv_obj_t *make_flip_tile(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *tile = ui_pixel_panel_create(parent, x, y, TILE_W, TILE_H, UI_PAPER);
    lv_obj_set_style_pad_all(tile, 0, 0);
    return tile;
}

void countdown_app_enter(void)
{
    countdown_init(&s_timer);
    s_beep_req = 0;
    s_done_flash_ms = 0;

    s_scr = ui_pixel_screen_create("TIMER");

    /* 像素电池：壳体 + 正极头，百分比写在壳内。 */
    px(s_scr, 186, 30, 32, 16, UI_INK);
    px(s_scr, 184, 28, 32, 16, UI_PAPER);
    px(s_scr, 216, 33, 4, 6, UI_INK);
    s_battery = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(s_battery, 184, 28);
    lv_obj_set_width(s_battery, 32);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_CENTER, 0);

    /* 内容区与草地之间的深色提示带，避免和卡片/吉祥物抢位。 */
    px(s_scr, 0, 210, 240, 24, UI_SKY_DARK);
    s_hint = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_width(s_hint, 228);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 213);

    s_select_group = make_group(s_scr);
    s_run_group = make_group(s_scr);

    for (int i = 0; i < COUNTDOWN_PRESET_COUNT; i++) {
        int x = 12 + (i % 2) * 110;
        int y = 50 + (i / 2) * 53;
        s_cards[i] = ui_pixel_panel_create(s_select_group, x, y, 104, 48, UI_PAPER);
        lv_obj_set_style_pad_all(s_cards[i], 0, 0);

        s_card_tabs[i] = px(s_cards[i], 0, 0, 6, 48, UI_ORANGE);

        s_card_nums[i] = ui_pixel_label(s_cards[i], PRESET_NUM[i],
                                        &lv_font_montserrat_20, UI_INK);
        lv_obj_align(s_card_nums[i], LV_ALIGN_TOP_MID, 3, 4);

        s_card_units[i] = ui_pixel_label(s_cards[i], PRESET_UNIT[i],
                                         &lv_font_montserrat_14, UI_INK);
        lv_obj_align(s_card_units[i], LV_ALIGN_BOTTOM_MID, 3, -3);
        lv_obj_set_style_text_opa(s_card_units[i], LV_OPA_70, 0);
    }

    s_time_panel = ui_pixel_panel_create(s_run_group, 10, 50, 220, 112, UI_PAPER);
    lv_obj_set_style_pad_all(s_time_panel, 4, 0);

    /* 翻页钟：四块数字牌 + 中间像素冒号，一眼能读剩余时间。 */
    const int clock_x = 8;
    const int clock_y = 8;
    const int gap = 6;
    for (int i = 0; i < 4; i++) {
        int x = clock_x + i * (TILE_W + gap);
        if (i >= 2) {
            x += 12;
        }
        s_tiles[i] = make_flip_tile(s_time_panel, x, clock_y);
        s_tile_labels[i] = ui_pixel_label(s_tiles[i], "0",
                                          &lv_font_montserrat_20, UI_INK);
        lv_obj_center(s_tile_labels[i]);
    }
    int colon_x = clock_x + 2 * (TILE_W + gap) + 2;
    s_colon_top = px(s_time_panel, colon_x, 22, 6, 6, UI_INK);
    s_colon_bot = px(s_time_panel, colon_x, 42, 6, 6, UI_INK);

    s_preset_caption = ui_pixel_label(s_time_panel, "",
                                      &lv_font_montserrat_14, UI_INK);
    lv_obj_align(s_preset_caption, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_opa(s_preset_caption, LV_OPA_70, 0);

    s_bar_track = ui_pixel_panel_create(s_run_group, 10, 168, 220, 32, UI_MUTED);
    lv_obj_set_style_pad_left(s_bar_track, 6, 0);
    lv_obj_set_style_pad_right(s_bar_track, 6, 0);
    s_bar_fill = px(s_bar_track, 6, 8, BAR_INNER_W, 12, UI_GRASS);

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 236);

    refresh_battery();
    refresh_ui();

    s_tick = lv_timer_create(on_tick, TICK_MS, NULL);
    if (!s_audio_task && s_audio_ok) {
        xTaskCreate(audio_task, "timer_beep", 4096, NULL, 4, &s_audio_task);
    }
    lv_screen_load(s_scr);
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
        memset(s_cards, 0, sizeof(s_cards));
        memset(s_card_nums, 0, sizeof(s_card_nums));
        memset(s_card_units, 0, sizeof(s_card_units));
        memset(s_card_tabs, 0, sizeof(s_card_tabs));
        memset(s_tiles, 0, sizeof(s_tiles));
        memset(s_tile_labels, 0, sizeof(s_tile_labels));
        s_select_group = s_run_group = NULL;
        s_time_panel = s_colon_top = s_colon_bot = NULL;
        s_preset_caption = s_bar_track = s_bar_fill = NULL;
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
