#pragma once

#include "lvgl.h"

#define UI_BG       0x0B1220
#define UI_PANEL    0x152033
#define UI_PANEL_HI 0x1E3148
#define UI_LINE     0x2A3F55
#define UI_INK      0x071018
#define UI_PAPER    0xD7E4F0
#define UI_CYAN     0x3DDCFF
#define UI_YELLOW   0xFFD928
#define UI_ORANGE   0xFF9F43
#define UI_RED      0xFF4D6D
#define UI_MUTED    0x7A8B9C

lv_obj_t *ui_pixel_screen_create(const char *title);
lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color);
lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color);
void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled);
