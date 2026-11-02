/*
 * 文件：bsp_led.c
 * 描述：使用独立 RMT 通道驱动五颗按键灯和六颗底部 RGB 灯。
 * 约束：底部灯动画仅由专用任务刷新，运行路径不动态分配。
 */

#include "bsp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bsp_config.h"
#include "bsp_internal.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// 10 MHz RMT 时钟下的编码资源配置，每个计数为 100 ns。
#define BSP_LED_RMT_RESOLUTION_HZ             (10000000U)
#define BSP_LED_RMT_MEM_BLOCK_SYMBOLS         (64U)
#define BSP_LED_RMT_QUEUE_DEPTH               (4U)
#define BSP_LED_BYTES_PER_PIXEL               (3U)

// 底部灯任务以 50 ms 周期推进白色呼吸和彩虹颜色轮。
#define BSP_BOTTOM_LED_TASK_STACK_SIZE        (2048U)
#define BSP_BOTTOM_LED_TASK_PRIORITY          (3U)
#define BSP_BOTTOM_LED_FRAME_INTERVAL_MS      (50U)
#define BSP_BOTTOM_LED_RAINBOW_STEP           (5U)
#define BSP_BOTTOM_LED_BREATH_MIN              (16U)
#define BSP_BOTTOM_LED_BREATH_STEP             (8U)
#define BSP_BOTTOM_LED_ERROR_LOG_INTERVAL_MS  (1000U)
#define BSP_LED_MAX_COLOR_VALUE               (255U)

// 单元素静态队列只保留最新完整灯效命令，避免快速调节阻塞控制任务。
#define BSP_BOTTOM_LED_COMMAND_QUEUE_LENGTH    (1U)

// 单组 WS2812 的固定硬件资源和像素缓冲区描述。
struct bsp_led_strip
{
    gpio_num_t data_gpio;
    size_t led_count;
    uint8_t *pixel_buffer;
    size_t pixel_buffer_size;
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    bool channel_enabled;
};

// 底部灯模式、纯色和亮度作为一个完整命令跨任务提交。
struct bsp_bottom_led_command
{
    enum bsp_bottom_led_mode mode;
    struct bsp_led_color color;
    uint8_t brightness;
};

static const char *tag = "bsp_led";
static uint8_t key_pixel_buffer[BSP_KEY_LED_COUNT * BSP_LED_BYTES_PER_PIXEL];
static uint8_t bottom_pixel_buffer[BSP_BOTTOM_LED_COUNT * BSP_LED_BYTES_PER_PIXEL];
static struct bsp_led_strip key_strip = {
    .data_gpio = BSP_KEY_LED_DATA_GPIO,
    .led_count = BSP_KEY_LED_COUNT,
    .pixel_buffer = key_pixel_buffer,
    .pixel_buffer_size = sizeof(key_pixel_buffer),
};
static struct bsp_led_strip bottom_strip = {
    .data_gpio = BSP_BOTTOM_LED_DATA_GPIO,
    .led_count = BSP_BOTTOM_LED_COUNT,
    .pixel_buffer = bottom_pixel_buffer,
    .pixel_buffer_size = sizeof(bottom_pixel_buffer),
};

// 底部动画任务使用静态栈和 TCB，句柄发布后不再销毁。
static StaticTask_t bottom_led_task_buffer;
static StackType_t bottom_led_task_stack[BSP_BOTTOM_LED_TASK_STACK_SIZE];
static TaskHandle_t bottom_led_task_handle;
static StaticQueue_t bottom_led_queue_buffer;
static uint8_t bottom_led_queue_storage[sizeof(struct bsp_bottom_led_command) *
                                        BSP_BOTTOM_LED_COMMAND_QUEUE_LENGTH];
static QueueHandle_t bottom_led_command_queue;

// WS2812 逻辑 0、逻辑 1 和帧复位对应的 RMT 波形。
static const rmt_symbol_word_t ws2812_zero = {
    .level0 = 1,
    .duration0 = 3U,
    .level1 = 0,
    .duration1 = 9U,
};

static const rmt_symbol_word_t ws2812_one = {
    .level0 = 1,
    .duration0 = 9U,
    .level1 = 0,
    .duration1 = 3U,
};

static const rmt_symbol_word_t ws2812_reset = {
    .level0 = 0,
    .duration0 = 400U,
    .level1 = 0,
    .duration1 = 400U,
};

// 将 GRB 字节流按最高位优先编码为 WS2812 RMT 符号。
static size_t encode_pixels(const void *data, size_t data_size,
                            size_t symbols_written, size_t symbols_free,
                            rmt_symbol_word_t *symbols, bool *done, void *context)
{
    size_t data_position = symbols_written / 8U;
    const uint8_t *bytes = data;

    (void)context;
    if (symbols_free < 8U)
    {
        return 0U;
    }
    if (data_position < data_size)
    {
        // 每次至少编码一个完整字节，避免位边界跨越 RMT 内存块。
        uint8_t value = bytes[data_position];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            symbols[bit] = (value & (0x80U >> bit)) != 0U ? ws2812_one : ws2812_zero;
        }
        return 8U;
    }

    // 所有像素完成后追加低电平复位码，使灯珠锁存本帧颜色。
    symbols[0] = ws2812_reset;
    *done = true;
    return 1U;
}

// 按 WS2812 要求的 GRB 顺序写入指定灯带的单颗像素。
static void set_pixel(struct bsp_led_strip *strip, size_t index,
                      struct bsp_led_color color)
{
    size_t offset = index * BSP_LED_BYTES_PER_PIXEL;

    strip->pixel_buffer[offset] = color.green;
    strip->pixel_buffer[offset + 1U] = color.red;
    strip->pixel_buffer[offset + 2U] = color.blue;
}

// 将同一颜色写入指定灯带的全部像素，不立即发送。
static void set_all_pixels(struct bsp_led_strip *strip, struct bsp_led_color color)
{
    for (size_t index = 0U; index < strip->led_count; index++)
    {
        set_pixel(strip, index, color);
    }
}

// 发送指定灯带的完整像素帧并等待完成，保证缓冲区可安全复用。
static esp_err_t refresh_strip(struct bsp_led_strip *strip)
{
    const rmt_transmit_config_t config = {
        .loop_count = 0,
    };

    if (strip->channel == NULL || strip->encoder == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = rmt_transmit(strip->channel, strip->encoder,
                                    strip->pixel_buffer, strip->pixel_buffer_size,
                                    &config);
    if (result != ESP_OK)
    {
        return result;
    }
    return rmt_tx_wait_all_done(strip->channel, -1);
}

// 逆序释放一组 RMT 资源，用于初始化失败后的完整回收。
static void cleanup_strip(struct bsp_led_strip *strip)
{
    if (strip->channel_enabled)
    {
        (void)rmt_disable(strip->channel);
        strip->channel_enabled = false;
    }
    if (strip->encoder != NULL)
    {
        (void)rmt_del_encoder(strip->encoder);
        strip->encoder = NULL;
    }
    if (strip->channel != NULL)
    {
        (void)rmt_del_channel(strip->channel);
        strip->channel = NULL;
    }
    memset(strip->pixel_buffer, 0, strip->pixel_buffer_size);
}

// 创建一组独立 RMT 通道和编码器，并同步发送全灭帧。
static esp_err_t init_strip(struct bsp_led_strip *strip)
{
    const rmt_simple_encoder_config_t encoder_config = {
        .callback = encode_pixels,
        .min_chunk_size = 8U,
    };
    rmt_tx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = strip->data_gpio,
        .mem_block_symbols = BSP_LED_RMT_MEM_BLOCK_SYMBOLS,
        .resolution_hz = BSP_LED_RMT_RESOLUTION_HZ,
        .trans_queue_depth = BSP_LED_RMT_QUEUE_DEPTH,
    };
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_ERROR(rmt_new_tx_channel(&channel_config, &strip->channel), error,
                      tag, "Create LED RMT channel failed");
    ESP_GOTO_ON_ERROR(rmt_new_simple_encoder(&encoder_config, &strip->encoder), error,
                      tag, "Create LED RMT encoder failed");
    ESP_GOTO_ON_ERROR(rmt_enable(strip->channel), error, tag,
                      "Enable LED RMT channel failed");
    strip->channel_enabled = true;
    memset(strip->pixel_buffer, 0, strip->pixel_buffer_size);
    ESP_GOTO_ON_ERROR(refresh_strip(strip), error, tag, "Clear LED strip failed");
    return ESP_OK;

error:
    cleanup_strip(strip);
    return ret;
}

// 生成与 v10 一致的三段 RGB 颜色轮。
static struct bsp_led_color get_rainbow_color(uint8_t position)
{
    struct bsp_led_color color = {0};

    if (position < 85U)
    {
        color.red = (uint8_t)(BSP_LED_MAX_COLOR_VALUE - position * 3U);
        color.green = (uint8_t)(position * 3U);
    }
    else if (position < 170U)
    {
        position = (uint8_t)(position - 85U);
        color.green = (uint8_t)(BSP_LED_MAX_COLOR_VALUE - position * 3U);
        color.blue = (uint8_t)(position * 3U);
    }
    else
    {
        position = (uint8_t)(position - 170U);
        color.red = (uint8_t)(position * 3U);
        color.blue = (uint8_t)(BSP_LED_MAX_COLOR_VALUE - position * 3U);
    }
    return color;
}

// 按全局亮度线性缩放单个颜色分量，整数舍入避免低亮度偏暗。
static uint8_t scale_color_component(uint8_t component, uint8_t brightness)
{
    uint16_t scaled = (uint16_t)component * brightness +
                      BSP_LED_MAX_COLOR_VALUE / 2U;

    return (uint8_t)(scaled / BSP_LED_MAX_COLOR_VALUE);
}

// 按全局亮度线性缩放 RGB 颜色。
static struct bsp_led_color scale_color(struct bsp_led_color color,
                                        uint8_t brightness)
{
    color.red = scale_color_component(color.red, brightness);
    color.green = scale_color_component(color.green, brightness);
    color.blue = scale_color_component(color.blue, brightness);
    return color;
}

// 生成全部灯珠同相位的青色呼吸帧，brightness 作为呼吸峰值。
static void set_bottom_breath_frame(uint8_t brightness, uint8_t breath_level)
{
    uint8_t breath_brightness = brightness > BSP_BOTTOM_LED_BREATH_MAX_BRIGHTNESS
                                    ? BSP_BOTTOM_LED_BREATH_MAX_BRIGHTNESS
                                    : brightness;
    uint8_t effective_brightness = scale_color_component(breath_brightness, breath_level);
    uint8_t red = BSP_LED_MAX_COLOR_VALUE -
                  scale_color_component(BSP_LED_MAX_COLOR_VALUE,
                                        BSP_BOTTOM_LED_BREATH_SATURATION);
    uint8_t green = BSP_LED_MAX_COLOR_VALUE;
    uint8_t blue = BSP_LED_MAX_COLOR_VALUE -
                   scale_color_component(25U, BSP_BOTTOM_LED_BREATH_SATURATION);
    struct bsp_led_color cyan = {
        .red = scale_color_component(red, effective_brightness),
        .green = scale_color_component(green, effective_brightness),
        .blue = scale_color_component(blue, effective_brightness),
    };

    set_all_pixels(&bottom_strip, cyan);
}

// 推进呼吸三角波，约三秒完成一个完整明暗周期。
static void advance_breath_level(uint8_t *breath_level, bool *increasing)
{
    if (*increasing)
    {
        if (*breath_level >= BSP_LED_MAX_COLOR_VALUE - BSP_BOTTOM_LED_BREATH_STEP)
        {
            *breath_level = BSP_LED_MAX_COLOR_VALUE;
            *increasing = false;
        }
        else
        {
            *breath_level = (uint8_t)(*breath_level + BSP_BOTTOM_LED_BREATH_STEP);
        }
    }
    else if (*breath_level <= BSP_BOTTOM_LED_BREATH_MIN + BSP_BOTTOM_LED_BREATH_STEP)
    {
        *breath_level = BSP_BOTTOM_LED_BREATH_MIN;
        *increasing = true;
    }
    else
    {
        *breath_level = (uint8_t)(*breath_level - BSP_BOTTOM_LED_BREATH_STEP);
    }
}

// 按底部灯数量在颜色轮上的均匀相位生成一帧彩虹流水。
static void set_bottom_flow_frame(uint8_t rainbow_offset, uint8_t brightness)
{
    for (size_t index = 0U; index < bottom_strip.led_count; index++)
    {
        uint8_t position = (uint8_t)(rainbow_offset +
                                     index * 256U / bottom_strip.led_count);
        struct bsp_led_color color = get_rainbow_color(position);
        set_pixel(&bottom_strip, index, scale_color(color, brightness));
    }
}

// 底部灯发送失败最多每秒记录一次，任务保持运行以便后续重试。
static void log_bottom_led_error(esp_err_t result, TickType_t *last_log_tick,
                                 bool *has_logged_error)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t log_interval = pdMS_TO_TICKS(BSP_BOTTOM_LED_ERROR_LOG_INTERVAL_MS);

    if (!*has_logged_error || now - *last_log_tick >= log_interval)
    {
        ESP_LOGE(tag, "Refresh bottom LEDs failed: %s", esp_err_to_name(result));
        *last_log_tick = now;
        *has_logged_error = true;
    }
}

// 独占底部缓冲区和 RMT 通道，按完整命令刷新动画或统一纯色帧。
static void bottom_led_task(void *argument)
{
    struct bsp_bottom_led_command current_command = {
        .mode = BSP_BOTTOM_LED_MODE_WHITE,
        .brightness = BSP_LED_MAX_COLOR_VALUE,
    };
    struct bsp_bottom_led_command new_command;
    uint8_t rainbow_offset = 0U;
    uint8_t breath_level = BSP_BOTTOM_LED_BREATH_MIN;
    bool command_received = false;
    bool breath_increasing = true;
    bool has_logged_error = false;
    TickType_t last_error_log_tick = 0U;

    (void)argument;
    for (;;)
    {
        bool animated_mode = current_command.mode == BSP_BOTTOM_LED_MODE_WHITE ||
                             current_command.mode == BSP_BOTTOM_LED_MODE_FLOW;
        TickType_t wait_ticks = command_received && animated_mode ?
                               pdMS_TO_TICKS(BSP_BOTTOM_LED_FRAME_INTERVAL_MS) :
                               portMAX_DELAY;
        BaseType_t received = xQueueReceive(bottom_led_command_queue, &new_command,
                                            wait_ticks);
        if (received == pdTRUE)
        {
            if (new_command.mode > BSP_BOTTOM_LED_MODE_SOLID)
            {
                continue;
            }
            if (!command_received ||
                (current_command.mode != BSP_BOTTOM_LED_MODE_WHITE &&
                 new_command.mode == BSP_BOTTOM_LED_MODE_WHITE))
            {
                breath_level = BSP_BOTTOM_LED_BREATH_MIN;
                breath_increasing = true;
            }
            current_command = new_command;
            command_received = true;
        }
        else if (!command_received || !animated_mode)
        {
            continue;
        }

        if (current_command.mode == BSP_BOTTOM_LED_MODE_WHITE)
        {
            set_bottom_breath_frame(current_command.brightness, breath_level);
            advance_breath_level(&breath_level, &breath_increasing);
        }
        else if (current_command.mode == BSP_BOTTOM_LED_MODE_CYAN)
        {
            struct bsp_led_color cyan = {
                .red = 0U,
                .green = BSP_LED_MAX_COLOR_VALUE,
                .blue = 230U,
            };
            set_all_pixels(&bottom_strip,
                           scale_color(cyan, current_command.brightness));
        }
        else if (current_command.mode == BSP_BOTTOM_LED_MODE_FLOW)
        {
            set_bottom_flow_frame(rainbow_offset, current_command.brightness);
            rainbow_offset = (uint8_t)(rainbow_offset + BSP_BOTTOM_LED_RAINBOW_STEP);
        }
        else
        {
            set_all_pixels(&bottom_strip,
                           scale_color(current_command.color,
                                       current_command.brightness));
        }

        esp_err_t result = refresh_strip(&bottom_strip);
        if (result != ESP_OK)
        {
            log_bottom_led_error(result, &last_error_log_tick, &has_logged_error);
        }
    }
}

// 初始化两组灯光资源，底部任务最后创建并接管底部灯。
esp_err_t bsp_led_init(void)
{
    ESP_RETURN_ON_FALSE(key_strip.channel == NULL && bottom_strip.channel == NULL &&
                        bottom_led_task_handle == NULL &&
                        bottom_led_command_queue == NULL,
                        ESP_ERR_INVALID_STATE, tag, "LED is already initialized");

    esp_err_t result = init_strip(&key_strip);
    if (result != ESP_OK)
    {
        return result;
    }

    result = init_strip(&bottom_strip);
    if (result != ESP_OK)
    {
        cleanup_strip(&key_strip);
        return result;
    }

    bottom_led_command_queue = xQueueCreateStatic(
        BSP_BOTTOM_LED_COMMAND_QUEUE_LENGTH,
        sizeof(struct bsp_bottom_led_command), bottom_led_queue_storage,
        &bottom_led_queue_buffer);
    if (bottom_led_command_queue == NULL)
    {
        cleanup_strip(&bottom_strip);
        cleanup_strip(&key_strip);
        return ESP_ERR_NO_MEM;
    }

    bottom_led_task_handle = xTaskCreateStatic(bottom_led_task, "bottom_led",
                                               BSP_BOTTOM_LED_TASK_STACK_SIZE, NULL,
                                               BSP_BOTTOM_LED_TASK_PRIORITY,
                                               bottom_led_task_stack,
                                               &bottom_led_task_buffer);
    if (bottom_led_task_handle == NULL)
    {
        bottom_led_command_queue = NULL;
        cleanup_strip(&bottom_strip);
        cleanup_strip(&key_strip);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(tag, "RGB LEDs initialized: keys=GPIO%d/%u, bottom=GPIO%d/%u",
             BSP_KEY_LED_DATA_GPIO, BSP_KEY_LED_COUNT,
             BSP_BOTTOM_LED_DATA_GPIO, BSP_BOTTOM_LED_COUNT);
    return ESP_OK;
}

// 更新五颗按键灯的独立颜色并发送完整帧。
esp_err_t bsp_led_show(const struct bsp_led_color *colors, size_t count)
{
    ESP_RETURN_ON_FALSE(colors != NULL && count == BSP_KEY_LED_COUNT,
                        ESP_ERR_INVALID_ARG, tag, "Invalid key LED colors");

    for (size_t index = 0U; index < count; index++)
    {
        set_pixel(&key_strip, index, colors[index]);
    }
    return refresh_strip(&key_strip);
}

// 将同一颜色写入全部按键灯并发送完整帧。
esp_err_t bsp_led_set_all(struct bsp_led_color color)
{
    set_all_pixels(&key_strip, color);
    return refresh_strip(&key_strip);
}

// 将最新的完整底部灯命令覆盖写入单元素静态队列。
static esp_err_t submit_bottom_led_command(
    const struct bsp_bottom_led_command *command)
{
    ESP_RETURN_ON_FALSE(bottom_led_task_handle != NULL &&
                        bottom_led_command_queue != NULL,
                        ESP_ERR_INVALID_STATE, tag,
                        "Bottom LED task is not ready");
    return xQueueOverwrite(bottom_led_command_queue, command) == pdPASS ?
           ESP_OK : ESP_FAIL;
}

// 覆盖提交底部灯模式和亮度，发送结果由底部任务异步处理。
esp_err_t bsp_bottom_led_set_state(enum bsp_bottom_led_mode mode,
                                   uint8_t brightness)
{
    ESP_RETURN_ON_FALSE(mode >= BSP_BOTTOM_LED_MODE_WHITE &&
                        mode <= BSP_BOTTOM_LED_MODE_FLOW,
                        ESP_ERR_INVALID_ARG, tag, "Invalid bottom LED mode");
    const struct bsp_bottom_led_command command = {
        .mode = mode,
        .brightness = brightness,
    };
    return submit_bottom_led_command(&command);
}

// 覆盖提交六颗底部灯的统一纯色和亮度。
esp_err_t bsp_bottom_led_set_solid_color(struct bsp_led_color color,
                                         uint8_t brightness)
{
    const struct bsp_bottom_led_command command = {
        .mode = BSP_BOTTOM_LED_MODE_SOLID,
        .color = color,
        .brightness = brightness,
    };
    return submit_bottom_led_command(&command);
}
