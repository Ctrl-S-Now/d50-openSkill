/*
 * app_main.c — D50 开源基础固件入口。
 *
 * 流程：初始化板级外设 → JOBY 标志淡入淡出 → 欢迎语与社区二维码 → 逐项外设自检。
 * 想在此基础上开发自己的应用：从 app_main() 末尾开始写，外设接口见 components/bsp/include/bsp.h。
 */
#include <stdio.h>
#include <string.h>

#include "boot_ui.h"
#include "bsp.h"
#include "esp_log.h"
#include "font_asset.h"
#include "led_fx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "selftest.h"
#include "sound.h"
#include "usb.h"
#include "wifi_manager.h"

static const char *tag = "d50_open";

// 输入回调运行在 BSP 输入任务里：按键灯随按键渐亮渐暗，旋钮按下有点击音。
static void on_input(enum bsp_input_event event, uint8_t index, void *context)
{
    (void)context;
    switch (event)
    {
    case BSP_INPUT_CLICK:
        if (index < BSP_KEY_COUNT)
        {
            led_fx_pulse(index, 120);   // 单击：渐亮后渐暗
        }
        break;
    case BSP_INPUT_LONG_PRESSED:
        if (index < BSP_KEY_COUNT)
        {
            led_fx_hold(index);         // 长按期间保持亮，松开后渐暗
        }
        break;
    case BSP_INPUT_LONG_RELEASED:
        led_fx_release_all();
        break;
    case BSP_INPUT_ENCODER_PRESSED:
        (void)sound_play_click();
        break;
    default:
        break;
    }
}

static void on_joystick(int8_t x, int8_t y, void *context)
{
    (void)x;
    (void)y;
    (void)context;
}

// USB 麦克风（UAC）数据源：直接取板载麦克风 PCM，失败时补静音。
static esp_err_t usb_mic_read(uint8_t *buffer, size_t size, size_t *read_size, void *context)
{
    (void)context;
    size_t actual = 0U;
    if (bsp_audio_read(buffer, size, &actual) != ESP_OK || actual != size)
    {
        memset(buffer, 0, size);
        actual = size;
    }
    *read_size = actual;
    return ESP_OK;
}

static void report(enum boot_item item, const struct selftest_result *result, bool *all_ok)
{
    ESP_LOGI(tag, "self-test #%d: %s %s", (int)item, result->ok ? "OK" : "FAIL", result->detail);
    all_ok[item] = result->ok;
    boot_ui_set_item(item, false, result->ok, result->detail);
}

#define FADE_MS   (450U)

// 渐暗 → 在全黑里换内容 → 渐亮：用背光 PWM 做页面切换，不硬切，也没有整屏半透明混合，不会闪。
static void transition(esp_err_t (*swap_screen)(void))
{
    (void)bsp_display_fade_brightness(0, FADE_MS);
    ESP_ERROR_CHECK(swap_screen());
    vTaskDelay(pdMS_TO_TICKS(120));   // 等新内容渲染完再亮起
    (void)bsp_display_fade_brightness(255, FADE_MS);
}

static const bool *home_results;
static esp_err_t swap_home(void)
{
    return boot_ui_show_home(home_results);
}

// Wi-Fi 配网页：初始化板载 Wi-Fi 链路，开热点和配网网页，成功后重启。
static void run_provisioning(void)
{
    transition(boot_ui_show_provision);
    boot_ui_set_provision("正在启动 Wi-Fi ...", "");
    struct selftest_result wifi;
    selftest_wifi(&wifi);
    if (!wifi.ok)
    {
        boot_ui_set_provision("Wi-Fi 芯片没有响应", "请参考 c2_firmware/README.md");
        return;
    }
    wifi_manager_start(boot_ui_set_provision, true);
}

void app_main(void)
{
    const struct bsp_config bsp_config = {
        .input_callback = on_input,
        .joystick_callback = on_joystick,
    };
    ESP_ERROR_CHECK(bsp_init(&bsp_config));
    ESP_ERROR_CHECK(led_fx_start());
    esp_err_t font_result = font_asset_init();
    if (font_result != ESP_OK)
    {
        ESP_LOGW(tag, "Full font unavailable (%s): Chinese text will be missing",
                 esp_err_to_name(font_result));
    }

    // 第 1 屏：标志。背光此时为 0，先在全黑里建好画面，再随背光缓缓亮起、停留、缓缓暗下。
    ESP_ERROR_CHECK(boot_ui_show_logo());
    vTaskDelay(pdMS_TO_TICKS(150));
    (void)bsp_display_fade_brightness(255, 900);
    vTaskDelay(pdMS_TO_TICKS(700));
    (void)bsp_display_fade_brightness(0, 700);

    // 只有打开了 D50_WIFI_PORTAL_ENABLE（open_config.h）且设备里还没保存 Wi-Fi 时，才进入配网页。
    // 平时（默认）固件的界面上不出现任何联网提示。
    if (wifi_manager_provision_requested())
    {
        run_provisioning();
        return;
    }

    // 第 2 屏：欢迎语。
    transition(boot_ui_show_welcome);
    vTaskDelay(pdMS_TO_TICKS(3000));   // 让三句话依次浮现并读完

    // 第 3 屏：自检清单，逐项出结果。
    transition(boot_ui_show_selftest);
    for (int item = 0; item < BOOT_ITEM_COUNT; item++)
    {
        boot_ui_set_item((enum boot_item)item, true, false, NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(600));

    bool all_ok[BOOT_ITEM_COUNT];
    struct selftest_result result;
    // 能走到这里说明屏幕正在显示。
    result = (struct selftest_result){.ok = true};
    report(BOOT_ITEM_DISPLAY, &result, all_ok);
    vTaskDelay(pdMS_TO_TICKS(250));

    selftest_leds(&result);
    report(BOOT_ITEM_LED, &result, all_ok);
    selftest_psram(&result);
    report(BOOT_ITEM_PSRAM, &result, all_ok);
    vTaskDelay(pdMS_TO_TICKS(200));
    // 扬声器：自检时播放提示音，能启动播放即视为通过。
    result = (struct selftest_result){.ok = sound_play_boot() == ESP_OK};
    report(BOOT_ITEM_SPEAKER, &result, all_ok);
    selftest_microphone(&result);   // 必须在 usb_init 之前：之后麦克风由 USB 音频占用
    report(BOOT_ITEM_MIC, &result, all_ok);
    // 按键/旋钮/摇杆需要人来操作，这里只确认输入任务已启动。
    result = (struct selftest_result){.ok = true};
    report(BOOT_ITEM_INPUT, &result, all_ok);

    const struct usb_config usb_config = {.audio_read_callback = usb_mic_read};
    result = (struct selftest_result){.ok = usb_init(&usb_config) == ESP_OK};
    report(BOOT_ITEM_USB, &result, all_ok);

    selftest_wifi(&result);
    report(BOOT_ITEM_WIFI, &result, all_ok);
    vTaskDelay(pdMS_TO_TICKS(1200));   // 留一点时间看完最后一项

    // 第 4 屏：首页。之后“最近输入”显示在这一屏底部。
    home_results = all_ok;
    transition(swap_home);
    if (all_ok[BOOT_ITEM_WIFI])
    {
        wifi_manager_start(NULL, false);   // 有 Wi-Fi 信息就静默连接；没有就保持离线
    }

    ESP_LOGI(tag, "Boot complete. Build your app from here.");
}
