// main/main.c —— 倒计时提醒玩法入口。
// 开机后直接进入 TIMER 页，不再经过基线硬件 demo 菜单。
// 保留 ui_pixel 主题与右上角电量；按键回调不阻塞，蜂鸣走工作任务。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "countdown_app.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include <stdbool.h>

static const char *TAG = "main";

// 按键回调运行在 button 组件任务里，操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!bsp_lvgl_lock(500)) {
        return;
    }
    countdown_app_key(btn, ev);
    bsp_lvgl_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "FoloToy countdown reminder 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，无法进入倒计时。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    bool button_ok = (bsp_button_init(on_key, NULL) == ESP_OK);
    bool audio_ok = (bsp_audio_init() == ESP_OK);
    bool battery_ok = (bsp_battery_init() == ESP_OK);
    countdown_app_set_peripherals(audio_ok, battery_ok);

    if (!button_ok) {
        ESP_LOGE(TAG, "按键初始化失败，倒计时无法操作");
    }

    if (bsp_lvgl_lock(1000)) {
        countdown_app_enter();
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "就绪: Button=%d Audio=%d Battery=%d",
             button_ok, audio_ok, battery_ok);
}
