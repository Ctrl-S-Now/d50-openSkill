/*
 * selftest.c — 开机外设自检。每个函数只做一件事并给出 ok/FAIL，不依赖 UI。
 */
#include "selftest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp.h"
#include "led_fx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

static const char *tag = "selftest";

static void set_result(struct selftest_result *out, bool ok, const char *detail)
{
    out->ok = ok;
    snprintf(out->detail, sizeof(out->detail), "%s", detail != NULL ? detail : "");
}

void selftest_leds(struct selftest_result *out)
{
    // 五颗按键灯依次渐亮、渐暗（由 led_fx 平滑过渡）；底部灯进入呼吸。
    for (uint8_t index = 0U; index < BSP_KEY_LED_COUNT; index++)
    {
        led_fx_pulse(index, 160);
        vTaskDelay(pdMS_TO_TICKS(180));
    }
    vTaskDelay(pdMS_TO_TICKS(500));   // 等最后一颗渐暗完
    esp_err_t bottom = bsp_bottom_led_set_state(BSP_BOTTOM_LED_MODE_WHITE, 255);
    set_result(out, bottom == ESP_OK, "");
}

void selftest_psram(struct selftest_result *out)
{
    size_t bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    char detail[sizeof(out->detail)];
    snprintf(detail, sizeof(detail), "%u MB", (unsigned)(bytes / (1024U * 1024U)));
    set_result(out, bytes > 0U, detail);
}

void selftest_microphone(struct selftest_result *out)
{
    int16_t buffer[320];
    size_t read_size = 0U;
    esp_err_t result = ESP_OK;
    // 丢弃前几帧启动瞬态，再取一帧。
    for (int attempt = 0; attempt < 6 && result == ESP_OK; attempt++)
    {
        result = bsp_audio_read(buffer, sizeof(buffer), &read_size);
    }
    set_result(out, result == ESP_OK && read_size > 0U, "");
}

void selftest_wifi(struct selftest_result *out)
{
    // 板载 ESP8684 经 SPI（esp-hosted）提供 Wi-Fi；扫描能返回结果即链路正常。
    uint16_t ap_count = 0U;
    esp_err_t result = wifi_manager_init();
    if (result == ESP_OK)
    {
        result = wifi_manager_scan(&ap_count);
    }
    if (result == ESP_OK)
    {
        char detail[sizeof(out->detail)];
        snprintf(detail, sizeof(detail), "%u APs", (unsigned)ap_count);
        set_result(out, true, detail);
    }
    else
    {
        ESP_LOGW(tag, "Wi-Fi self-test failed: %s", esp_err_to_name(result));
        set_result(out, false, esp_err_to_name(result));
    }
}
