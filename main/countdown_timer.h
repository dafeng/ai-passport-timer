#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 预设档位数：30s / 5m / 10m / 15m / 20m / 30m。 */
#define COUNTDOWN_PRESET_COUNT 6

/* 各预设时长（毫秒）。与 COUNTDOWN_PRESET_LABELS 按下标对齐。 */
extern const uint32_t COUNTDOWN_PRESET_MS[COUNTDOWN_PRESET_COUNT];

/* 预设英文短标签，供 UI 使用。基线字体无 CJK，必须用 ASCII。 */
extern const char *const COUNTDOWN_PRESET_LABELS[COUNTDOWN_PRESET_COUNT];

/*
 * 倒计时状态机：
 *   SELECT  — 选择预设，尚未开始
 *   RUNNING — 正在递减
 *   PAUSED  — 已暂停，remaining 冻结
 *   DONE    — 到 0，等待用户确认后回到 SELECT
 */
typedef enum {
    COUNTDOWN_STATE_SELECT = 0,
    COUNTDOWN_STATE_RUNNING,
    COUNTDOWN_STATE_PAUSED,
    COUNTDOWN_STATE_DONE,
} countdown_state_t;

/*
 * 纯逻辑倒计时模型，不依赖 ESP-IDF / LVGL。
 * remaining 由单调时钟差值计算，避免 lv_timer 抖动累计误差。
 *
 * 跨任务：本结构由 UI 任务独占读写；按键回调若与 tick 同持 LVGL 锁则无需额外同步。
 */
typedef struct {
    countdown_state_t state;          /* 当前状态 */
    int preset_index;                 /* 0 .. COUNTDOWN_PRESET_COUNT-1 */
    uint32_t duration_ms;             /* 本次会话总时长 */
    uint32_t remaining_ms;            /* 当前剩余；DONE 时为 0 */
    uint32_t remaining_at_anchor_ms;  /* 最近一次 start/resume 时的剩余 */
    uint32_t anchor_ms;               /* 最近一次 start/resume 的单调时钟（ms） */
} countdown_t;

/* 初始化为 SELECT、第 0 档（30s）。t 不得为 NULL。 */
void countdown_init(countdown_t *t);

/* 仅 SELECT 下循环切换预设；其它状态忽略。 */
void countdown_select_next(countdown_t *t);
void countdown_select_prev(countdown_t *t);

/*
 * SELECT 下按当前预设开始计时。now_ms 为调用方提供的单调毫秒。
 * 其它状态忽略。
 */
void countdown_start(countdown_t *t, uint32_t now_ms);

/* RUNNING 下先同步 remaining 再进入 PAUSED；其它状态忽略。 */
void countdown_pause(countdown_t *t, uint32_t now_ms);

/* PAUSED 下从当前 remaining 继续；其它状态忽略。 */
void countdown_resume(countdown_t *t, uint32_t now_ms);

/* 任意状态回到 SELECT，remaining 恢复为当前预设时长。 */
void countdown_reset(countdown_t *t);

/*
 * RUNNING 下按 now_ms 更新 remaining。
 * 若本拍转入 DONE 返回 true，便于 UI 触发提醒；否则 false。
 */
bool countdown_tick(countdown_t *t, uint32_t now_ms);

/*
 * 剩余时间占 duration 的千分比 0..1000；duration 为 0 时返回 0。
 * 用于进度条：开始=1000，结束=0。
 */
uint32_t countdown_remaining_permille(const countdown_t *t);

/*
 * 将 remaining 格式化为 "MM:SS"。
 * 显示用向上取整秒，避免最后 1ms~999ms 闪成 00:00 却尚未 DONE。
 * buf 至少 6 字节（含 NUL）。
 */
void countdown_format_mmss(uint32_t remaining_ms, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
