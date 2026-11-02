/*
 * led_fx.c — 按键灯渐亮渐暗。后台任务每 32 ms 把“当前亮度”向“目标亮度”推进一小步。
 * 亮的时候快（约 120 ms 到顶），暗的时候慢（约 500 ms 到底），观感更柔和。
 * 每颗灯可以有自己的颜色和常亮底光。
 * 灯带每发一帧都有极小概率出错，所以只在亮度变化时才发（约 30 帧/秒），
 * 渐变结束后补发一帧确认最终状态；没有变化时一帧都不发。
 */
#include "led_fx.h"

#include <stdbool.h>

#include "bsp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TICK_MS        (32U)
#define RISE_STEP      (68)    // 255 / (120 ms / 32 ms)
#define FALL_STEP      (16)    // 255 / (500 ms / 32 ms)

// 默认颜色：青蓝，亮度 0…255 按比例缩放。
static const struct bsp_led_color default_color = {.red = 0, .green = 100, .blue = 140};

static struct bsp_led_color key_color[BSP_KEY_LED_COUNT];
static volatile uint8_t floor_level[BSP_KEY_LED_COUNT];    // 平时的常亮底光
static volatile int target[BSP_KEY_LED_COUNT];
static volatile bool latched[BSP_KEY_LED_COUNT];           // 长按期间保持
static volatile int64_t pulse_until_ms[BSP_KEY_LED_COUNT];
static int current[BSP_KEY_LED_COUNT];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void led_fx_hold(uint8_t index)
{
    if (index < BSP_KEY_LED_COUNT)
    {
        latched[index] = true;
        target[index] = 255;
    }
}

void led_fx_release_all(void)
{
    for (int index = 0; index < BSP_KEY_LED_COUNT; index++)
    {
        latched[index] = false;
        pulse_until_ms[index] = 0;
        target[index] = floor_level[index];
    }
}

void led_fx_pulse(uint8_t index, uint32_t hold_ms)
{
    if (index < BSP_KEY_LED_COUNT)
    {
        pulse_until_ms[index] = now_ms() + (int64_t)hold_ms;
        target[index] = 255;
    }
}

void led_fx_set_glow(uint8_t index, struct bsp_led_color color, uint8_t level)
{
    if (index < BSP_KEY_LED_COUNT)
    {
        key_color[index] = color;
        floor_level[index] = level;
        if (!latched[index] && now_ms() >= pulse_until_ms[index])
        {
            target[index] = level;
        }
    }
}

static void fx_task(void *argument)
{
    (void)argument;
    bool was_changing = false;
    for (;;)
    {
        bool changed = false;
        struct bsp_led_color colors[BSP_KEY_LED_COUNT];
        int64_t now = now_ms();
        for (int index = 0; index < BSP_KEY_LED_COUNT; index++)
        {
            // 点按的保持时间到了且没有被长按锁住，就回到底光亮度（渐变）。
            if (!latched[index] && target[index] != floor_level[index] && now >= pulse_until_ms[index])
            {
                target[index] = floor_level[index];
            }
            int goal = target[index];
            int before = current[index];
            if (before < goal)
            {
                current[index] = before + RISE_STEP > goal ? goal : before + RISE_STEP;
            }
            else if (before > goal)
            {
                current[index] = before - FALL_STEP < goal ? goal : before - FALL_STEP;
            }
            changed = changed || current[index] != before;
            colors[index].red = (uint8_t)(key_color[index].red * current[index] / 255);
            colors[index].green = (uint8_t)(key_color[index].green * current[index] / 255);
            colors[index].blue = (uint8_t)(key_color[index].blue * current[index] / 255);
        }
        // 变化中每帧都发；刚停止变化的那一刻再补发一帧，确保最终状态正确。
        if (changed || was_changing)
        {
            (void)bsp_led_show(colors, BSP_KEY_LED_COUNT);
        }
        was_changing = changed;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t led_fx_start(void)
{
    for (int index = 0; index < BSP_KEY_LED_COUNT; index++)
    {
        key_color[index] = default_color;
    }
    return xTaskCreate(fx_task, "led_fx", 3072, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
