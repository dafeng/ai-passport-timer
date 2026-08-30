#pragma once

#include "bsp_button.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 在 enter 之前调用，告知音频/电量是否可用。 */
void countdown_app_set_peripherals(bool audio_ok, bool battery_ok);

/*
 * 倒计时提醒页：开机后直接进入，不再经过基线硬件 demo 菜单。
 * 必须先完成 display / LVGL / button 初始化；调用方需已持有 bsp_lvgl_lock()。
 */
void countdown_app_enter(void);

/* 停音频任务与 LVGL 定时器，再删屏。必须在持锁下调用。 */
void countdown_app_exit(void);

/*
 * 按键处理。运行于 button 组件任务，调用方已持锁。
 * 慢操作（蜂鸣）只置位，由音频工作任务执行。
 */
void countdown_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);

#ifdef __cplusplus
}
#endif
