/*
 * 文件：bsp.h
 * 描述：D50 板级支持包公共接口，封装显示、输入、音频和 RGB 灯能力。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 板载音频固定采用 16 kHz、16 位、单声道 PCM 数据。
#define BSP_AUDIO_SAMPLE_RATE_HZ              (16000U)
#define BSP_AUDIO_BITS_PER_SAMPLE             (16U)
#define BSP_AUDIO_CHANNEL_COUNT               (1U)
#define BSP_AUDIO_BYTES_PER_SAMPLE            (BSP_AUDIO_BITS_PER_SAMPLE / 8U)

// 主板包含五个普通按键和五颗串联 RGB 按键灯。
#define BSP_KEY_COUNT                         (5U)
#define BSP_KEY_LED_COUNT                     (5U)

// 不适用按键索引的输入事件使用该值。
#define BSP_INPUT_INDEX_NONE                  (UINT8_MAX)
#define BSP_INPUT_INDEX_POP                   (UINT8_MAX - 1U)

#ifdef __cplusplus
extern "C" {
#endif

// BSP 向应用层上报的离散输入事件，摇杆仅在方向或回中状态变化时上报。
enum bsp_input_event
{
    BSP_INPUT_ENCODER_CLOCKWISE,
    BSP_INPUT_ENCODER_COUNTER_CLOCKWISE,
    BSP_INPUT_ENCODER_PRESSED,
    BSP_INPUT_ENCODER_RELEASED,
    BSP_INPUT_CLICK,
    BSP_INPUT_LONG_PRESSED,
    BSP_INPUT_LONG_RELEASED,
    BSP_INPUT_JOYSTICK_UP,
    BSP_INPUT_JOYSTICK_DOWN,
    BSP_INPUT_JOYSTICK_LEFT,
    BSP_INPUT_JOYSTICK_RIGHT,
    BSP_INPUT_JOYSTICK_CENTER,
};

/**
 * @brief 板级输入事件回调。
 * @param event 输入事件类型。
 * @param index 普通按键索引，范围为 0 到 BSP_KEY_COUNT - 1；POP 使用
 *              BSP_INPUT_INDEX_POP；其他事件为 BSP_INPUT_INDEX_NONE。
 * @param context 调用方上下文。
 */
typedef void (*bsp_input_callback_t)(enum bsp_input_event event,
                                     uint8_t index,
                                     void *context);

/**
 * @brief 板级摇杆二维坐标回调。
 * @param x 水平坐标，范围为 -100 到 100，负值向左。
 * @param y 垂直坐标，范围为 -100 到 100，负值向上。
 * @param context 调用方上下文。
 */
typedef void (*bsp_joystick_callback_t)(int8_t x, int8_t y, void *context);

// BSP 初始化配置，输入回调由输入任务上下文调用。
struct bsp_config
{
    bsp_input_callback_t input_callback;  // 输入事件回调，由 BSP 输入任务调用
    void *input_context;                  // 输入事件回调的调用方上下文
    bsp_joystick_callback_t joystick_callback;  // 摇杆二维坐标回调
    void *joystick_context;                      // 摇杆坐标回调上下文
};

// 单颗按键灯的 RGB 颜色分量。
struct bsp_led_color
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

// 底部六颗 RGB 灯当前支持的页面联动模式，白色模式为呼吸效果。
enum bsp_bottom_led_mode
{
    BSP_BOTTOM_LED_MODE_WHITE,
    BSP_BOTTOM_LED_MODE_CYAN,
    BSP_BOTTOM_LED_MODE_FLOW,
    BSP_BOTTOM_LED_MODE_SOLID,
};

/**
 * @brief 初始化已经确认的主板外设。
 * @param config 板级输入回调配置。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
esp_err_t bsp_init(const struct bsp_config *config);

/**
 * @brief 从 EEPROM 指定地址读取数据。
 * @param address EEPROM 起始地址。
 * @param data 接收数据的缓冲区。
 * @param size 读取字节数。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
esp_err_t bsp_eeprom_read(uint16_t address, void *data, size_t size);

/**
 * @brief 向 EEPROM 指定地址写入数据。
 * @param address EEPROM 起始地址。
 * @param data 待写入数据的缓冲区。
 * @param size 写入字节数。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 * @note 驱动会按 EEPROM 页边界拆分写入，并在操作后恢复硬件写保护。
 */
esp_err_t bsp_eeprom_write(uint16_t address, const void *data, size_t size);

/**
 * @brief 设置显示背光，开启时机由应用控制。
 * @note BSP 初始化后背光默认关闭；本接口在任务上下文调用。
 * @param enabled true 开启背光，false 关闭背光。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
esp_err_t bsp_display_set_backlight(bool enabled);

/**
 * @brief 读取普通按键 K1…K5 此刻是否被按下（不去抖，适合游戏等需要按下瞬间响应的场景）。
 * @param index 按键索引 0…BSP_KEY_COUNT-1。
 */
bool bsp_key_is_pressed(uint8_t index);

/**
 * @brief 设置背光亮度（PWM 调光）。
 * @param level 0 为熄灭，255 为最亮；内部按 γ=2 映射，观感接近线性。
 */
esp_err_t bsp_display_set_brightness(uint8_t level);

/**
 * @brief 把背光亮度线性渐变到目标值，阻塞约 duration_ms 后返回。
 * @note 用于页面切换的渐亮渐暗：先渐暗到 0，在全黑里换内容，再渐亮，不需要整屏半透明混合。
 */
esp_err_t bsp_display_fade_brightness(uint8_t target, uint32_t duration_ms);

/**
 * @brief 访问 LVGL 对象前获取显示互斥锁。
 * @param timeout_ms 超时时间，单位为毫秒；-1 表示永久等待。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
esp_err_t bsp_display_lock(int timeout_ms);

/**
 * @brief 释放显示互斥锁。
 */
void bsp_display_unlock(void);

/**
 * @brief 从板载麦克风读取单声道 PCM 数据。
 * @param buffer 接收缓冲区。
 * @param size 期望读取的字节数。
 * @param read_size 实际读取的字节数。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
esp_err_t bsp_audio_read(void *buffer, size_t size, size_t *read_size);

/**
 * @brief 向扬声器通路写入单声道 PCM 数据。
 * @param buffer PCM 数据缓冲区。
 * @param size 期望写入的字节数。
 * @param write_size 实际写入的字节数。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
esp_err_t bsp_audio_write(const void *buffer, size_t size, size_t *write_size);

/**
 * @brief 开启或静音编解码器与功放输出。
 * @param enabled true 表示开启输出，false 表示静音。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
esp_err_t bsp_audio_set_speaker_enabled(bool enabled);

/**
 * @brief 分别设置五颗按键灯的 RGB 颜色。
 * @param colors 颜色数组，必须包含 BSP_KEY_LED_COUNT 项。
 * @param count colors 数组中的元素数量。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
esp_err_t bsp_led_show(const struct bsp_led_color *colors, size_t count);

/**
 * @brief 将五颗按键灯设置为同一种 RGB 颜色。
 * @param color 目标颜色。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
esp_err_t bsp_led_set_all(struct bsp_led_color color);

/**
 * @brief 异步设置底部六颗 RGB 灯的显示模式和全局亮度。
 * @param mode 目标灯效模式。
 * @param brightness 全局亮度，0 表示熄灭，255 表示最大亮度。
 * @return 请求接受返回 ESP_OK；参数无效或灯效任务未就绪时返回错误。
 * @note 只能从任务上下文调用；成功不表示异步 RMT 帧已经发送完成。
 */
esp_err_t bsp_bottom_led_set_state(enum bsp_bottom_led_mode mode,
                                   uint8_t brightness);

/**
 * @brief 异步设置底部六颗 RGB 灯的统一纯色和亮度。
 * @param color 目标 RGB 颜色。
 * @param brightness 全局亮度，0 表示熄灭，255 表示最大亮度。
 * @return 请求接受返回 ESP_OK；灯效任务未就绪时返回错误。
 * @note 只能从任务上下文调用；新请求会覆盖尚未处理的旧请求。
 */
esp_err_t bsp_bottom_led_set_solid_color(struct bsp_led_color color,
                                         uint8_t brightness);

#ifdef __cplusplus
}
#endif
