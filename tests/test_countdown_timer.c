#include <assert.h>
#include <string.h>

#include "countdown_timer.h"

static void test_init_defaults(void)
{
    countdown_t t;
    countdown_init(&t);
    assert(t.state == COUNTDOWN_STATE_SELECT);
    assert(t.preset_index == 0);
    assert(t.duration_ms == 5u * 60u * 1000u);
    assert(t.remaining_ms == 5u * 60u * 1000u);
    assert(strcmp(COUNTDOWN_PRESET_LABELS[0], "5m") == 0);
    assert(strcmp(COUNTDOWN_PRESET_LABELS[4], "25m") == 0);
    assert(COUNTDOWN_PRESET_MS[4] == 25u * 60u * 1000u);
    assert(COUNTDOWN_PRESET_MS[5] == 30u * 60u * 1000u);
}

static void test_select_wraps(void)
{
    countdown_t t;
    countdown_init(&t);

    countdown_select_prev(&t);
    assert(t.preset_index == 5);
    assert(t.duration_ms == 30u * 60u * 1000u);

    countdown_select_next(&t);
    assert(t.preset_index == 0);

    for (int i = 0; i < 3; i++) {
        countdown_select_next(&t);
    }
    assert(t.preset_index == 3);
    assert(t.duration_ms == 20u * 60u * 1000u);
}

static void test_select_ignored_while_running(void)
{
    countdown_t t;
    countdown_init(&t);
    countdown_start(&t, 1000);
    countdown_select_next(&t);
    assert(t.preset_index == 0);
    assert(t.state == COUNTDOWN_STATE_RUNNING);
}

static void test_run_pause_resume(void)
{
    countdown_t t;
    countdown_init(&t);
    countdown_start(&t, 0);
    assert(!countdown_tick(&t, 10000));
    assert(t.remaining_ms == 290000);
    assert(countdown_remaining_permille(&t) == 966);

    countdown_pause(&t, 10000);
    assert(t.state == COUNTDOWN_STATE_PAUSED);
    assert(t.remaining_ms == 290000);
    assert(!countdown_tick(&t, 50000));
    assert(t.remaining_ms == 290000);

    countdown_resume(&t, 50000);
    assert(t.state == COUNTDOWN_STATE_RUNNING);
    assert(!countdown_tick(&t, 60000));
    assert(t.remaining_ms == 280000);
}

static void test_expires_to_done(void)
{
    countdown_t t;
    countdown_init(&t);
    countdown_start(&t, 0);
    assert(countdown_tick(&t, 5u * 60u * 1000u));
    assert(t.state == COUNTDOWN_STATE_DONE);
    assert(t.remaining_ms == 0);
    assert(countdown_remaining_permille(&t) == 0);
    assert(!countdown_tick(&t, 40000));
}

static void test_reset_returns_to_select(void)
{
    countdown_t t;
    countdown_init(&t);
    countdown_select_next(&t);
    countdown_start(&t, 0);
    (void)countdown_tick(&t, 1000);
    countdown_reset(&t);
    assert(t.state == COUNTDOWN_STATE_SELECT);
    assert(t.preset_index == 1);
    assert(t.remaining_ms == COUNTDOWN_PRESET_MS[1]);
}

static void test_format_mmss(void)
{
    char buf[8];

    countdown_format_mmss(0, buf, sizeof(buf));
    assert(strcmp(buf, "00:00") == 0);

    countdown_format_mmss(1, buf, sizeof(buf));
    assert(strcmp(buf, "00:01") == 0);

    countdown_format_mmss(30000, buf, sizeof(buf));
    assert(strcmp(buf, "00:30") == 0);

    countdown_format_mmss(5u * 60u * 1000u, buf, sizeof(buf));
    assert(strcmp(buf, "05:00") == 0);

    countdown_format_mmss(30u * 60u * 1000u, buf, sizeof(buf));
    assert(strcmp(buf, "30:00") == 0);
}

int main(void)
{
    test_init_defaults();
    test_select_wraps();
    test_select_ignored_while_running();
    test_run_pause_resume();
    test_expires_to_done();
    test_reset_returns_to_select();
    test_format_mmss();
    return 0;
}
