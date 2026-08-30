#include "countdown_timer.h"

#include <stdbool.h>
#include <stdio.h>

const uint32_t COUNTDOWN_PRESET_MS[COUNTDOWN_PRESET_COUNT] = {
    30u * 1000u,
    5u * 60u * 1000u,
    10u * 60u * 1000u,
    15u * 60u * 1000u,
    20u * 60u * 1000u,
    30u * 60u * 1000u,
};

const char *const COUNTDOWN_PRESET_LABELS[COUNTDOWN_PRESET_COUNT] = {
    "30s", "5m", "10m", "15m", "20m", "30m",
};

static void apply_preset(countdown_t *t)
{
    t->duration_ms = COUNTDOWN_PRESET_MS[t->preset_index];
    t->remaining_ms = t->duration_ms;
    t->remaining_at_anchor_ms = t->duration_ms;
    t->anchor_ms = 0;
}

void countdown_init(countdown_t *t)
{
    t->state = COUNTDOWN_STATE_SELECT;
    t->preset_index = 0;
    apply_preset(t);
}

void countdown_select_next(countdown_t *t)
{
    if (t->state != COUNTDOWN_STATE_SELECT) {
        return;
    }
    t->preset_index = (t->preset_index + 1) % COUNTDOWN_PRESET_COUNT;
    apply_preset(t);
}

void countdown_select_prev(countdown_t *t)
{
    if (t->state != COUNTDOWN_STATE_SELECT) {
        return;
    }
    t->preset_index = (t->preset_index + COUNTDOWN_PRESET_COUNT - 1) %
                      COUNTDOWN_PRESET_COUNT;
    apply_preset(t);
}

void countdown_start(countdown_t *t, uint32_t now_ms)
{
    if (t->state != COUNTDOWN_STATE_SELECT) {
        return;
    }
    apply_preset(t);
    t->anchor_ms = now_ms;
    t->state = COUNTDOWN_STATE_RUNNING;
}

void countdown_pause(countdown_t *t, uint32_t now_ms)
{
    if (t->state != COUNTDOWN_STATE_RUNNING) {
        return;
    }
    (void)countdown_tick(t, now_ms);
    if (t->state == COUNTDOWN_STATE_RUNNING) {
        t->state = COUNTDOWN_STATE_PAUSED;
    }
}

void countdown_resume(countdown_t *t, uint32_t now_ms)
{
    if (t->state != COUNTDOWN_STATE_PAUSED) {
        return;
    }
    t->remaining_at_anchor_ms = t->remaining_ms;
    t->anchor_ms = now_ms;
    t->state = COUNTDOWN_STATE_RUNNING;
}

void countdown_reset(countdown_t *t)
{
    t->state = COUNTDOWN_STATE_SELECT;
    apply_preset(t);
}

bool countdown_tick(countdown_t *t, uint32_t now_ms)
{
    if (t->state != COUNTDOWN_STATE_RUNNING) {
        return false;
    }

    uint32_t elapsed = now_ms - t->anchor_ms;
    if (elapsed >= t->remaining_at_anchor_ms) {
        t->remaining_ms = 0;
        t->state = COUNTDOWN_STATE_DONE;
        return true;
    }

    t->remaining_ms = t->remaining_at_anchor_ms - elapsed;
    return false;
}

uint32_t countdown_remaining_permille(const countdown_t *t)
{
    if (t->duration_ms == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)t->remaining_ms * 1000u) / t->duration_ms);
}

void countdown_format_mmss(uint32_t remaining_ms, char *buf, size_t len)
{
    uint32_t total_s = 0;
    if (remaining_ms > 0) {
        /* 向上取整：1..1000ms 显示 00:01，到 0 才显示 00:00。 */
        total_s = (remaining_ms + 999u) / 1000u;
    }
    uint32_t mm = total_s / 60u;
    uint32_t ss = total_s % 60u;
    (void)snprintf(buf, len, "%02u:%02u", (unsigned)mm, (unsigned)ss);
}
