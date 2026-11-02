/*
 * 文件：bsp_input.c
 * 描述：轮询旋钮、五键、POP 和摇杆，完成 GPIO 去抖、正交解码与 ADC 死区处理。
 * 约束：输入任务只上报事件，不直接修改应用状态或操作 LVGL。
 */

#include "bsp.h"

#include <stdint.h>
#include <stdlib.h>

#include "bsp_config.h"
#include "bsp_internal.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

// GPIO 输入每 1 ms 采样一次，连续稳定 20 ms 后才上报按键动作。
#define BSP_INPUT_SAMPLE_PERIOD_MS            (1U)
#define BSP_BUTTON_DEBOUNCE_MS                (20U)
#define BSP_BUTTON_LONG_PRESS_MS              (1000U)
#define BSP_GPIO_INPUT_TASK_STACK_SIZE        (3072U)

// 摇杆使用多次采样均值，并保留中心死区以抑制机械漂移。
#define BSP_JOYSTICK_TASK_STACK_SIZE          (2048U)
#define BSP_JOYSTICK_SAMPLE_PERIOD_MS         (50U)
#define BSP_JOYSTICK_SAMPLE_COUNT             (16U)
#define BSP_JOYSTICK_CENTER                   (1750)
#define BSP_JOYSTICK_DEAD_ZONE                (250)
#define BSP_JOYSTICK_RAW_MIN                  (0)
#define BSP_JOYSTICK_RAW_MAX                  (3500)
#define BSP_JOYSTICK_OUTPUT_MAX               (100)

// 摇杆离散方向状态，NEUTRAL 用于识别离开死区和回中边沿。
enum bsp_joystick_state
{
    BSP_JOYSTICK_NEUTRAL,
    BSP_JOYSTICK_UP,
    BSP_JOYSTICK_DOWN,
    BSP_JOYSTICK_LEFT,
    BSP_JOYSTICK_RIGHT,
};

// 单个低电平有效按键的软件去抖状态。
struct button_state
{
    gpio_num_t gpio;          // 按键对应的 GPIO
    uint8_t stable_level;     // 已确认并上报的稳定电平
    uint8_t sampled_level;    // 当前消抖候选电平
    uint32_t stable_time_ms;  // 候选电平已经保持的时间
    TickType_t pressed_tick;
    bool long_pressed;
};

// 普通按键索引与原理图 KEY1 到 KEY5 的顺序保持一致。
static const gpio_num_t key_gpios[BSP_KEY_COUNT] = {
    BSP_KEY_1_GPIO,
    BSP_KEY_2_GPIO,
    BSP_KEY_3_GPIO,
    BSP_KEY_4_GPIO,
    BSP_KEY_5_GPIO,
};

bool bsp_key_is_pressed(uint8_t index)
{
    return index < BSP_KEY_COUNT && gpio_get_level(key_gpios[index]) == 0;
}

// 日志名称与普通按键索引保持一致，便于烧录后逐键检查。
static const char *const key_names[BSP_KEY_COUNT] = {
    "KEY1",
    "KEY2",
    "KEY3",
    "KEY4",
    "KEY5",
};

static const char *tag = "bsp_input";
// 回调及上下文由应用层注册，在两个输入任务中只读使用。
static bsp_input_callback_t input_callback;
static void *input_context;
static bsp_joystick_callback_t joystick_callback;
static void *joystick_context;
// ADC 和任务句柄用于持有输入驱动资源并防止重复初始化。
static adc_oneshot_unit_handle_t adc_handle;
static TaskHandle_t gpio_input_task_handle;
static TaskHandle_t joystick_task_handle;

// 在输入任务上下文同步通知应用层。
static void emit_event(enum bsp_input_event event, uint8_t index)
{
    if (input_callback != NULL)
    {
        input_callback(event, index, input_context);
    }
}

// 在摇杆任务上下文同步发布死区处理后的二维坐标。
static void emit_joystick_position(int8_t x, int8_t y)
{
    if (joystick_callback != NULL)
    {
        joystick_callback(x, y, joystick_context);
    }
}

// 将旋钮 A/B 两相电平合并为 2 位格雷码相位。
static uint8_t read_encoder_phase(void)
{
    return (uint8_t)((gpio_get_level(BSP_ENCODER_PHASE_A_GPIO) << 1U) |
                     gpio_get_level(BSP_ENCODER_PHASE_B_GPIO));
}

// 以当前 GPIO 电平初始化去抖状态，避免开机时误报一次边沿。
static void init_button_state(struct button_state *state, gpio_num_t gpio)
{
    uint8_t level = (uint8_t)gpio_get_level(gpio);

    state->gpio = gpio;
    state->stable_level = level;
    state->sampled_level = level;
    state->stable_time_ms = 0U;
    state->pressed_tick = level == 0U ? xTaskGetTickCount() : 0U;
    state->long_pressed = false;
}

/*
 * 更新按键去抖状态。
 * 仅当候选电平连续稳定达到阈值且不同于已上报电平时返回 true。
 */
static bool update_button_state(struct button_state *state)
{
    uint8_t current_level = (uint8_t)gpio_get_level(state->gpio);

    if (current_level != state->sampled_level)
    {
        // 电平再次变化时重新累计稳定时间。
        state->sampled_level = current_level;
        state->stable_time_ms = 0U;
        return false;
    }
    if (state->stable_time_ms < BSP_BUTTON_DEBOUNCE_MS)
    {
        state->stable_time_ms += BSP_INPUT_SAMPLE_PERIOD_MS;
    }
    if (state->stable_time_ms >= BSP_BUTTON_DEBOUNCE_MS &&
        state->stable_level != state->sampled_level)
    {
        state->stable_level = state->sampled_level;
        return true;
    }
    return false;
}

// 使用 4 位状态转移表解码旋钮，并以完整卡点为单位上报方向。
static void scan_encoder(int8_t *transition_count, uint8_t *previous_phase)
{
    // 高两位为上一相位，低两位为当前相位；非法跳变对应 0。
    static const int8_t transition_table[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0,
    };
    uint8_t current_phase = read_encoder_phase();
    int8_t step = transition_table[(*previous_phase << 2U) | current_phase];

#if BSP_ENCODER_DIRECTION_INVERTED
    // 新编码器 A/B 相序与原 30 步编码器相反，统一为应用层既有方向语义。
    step = -step;
#endif

    if (step != 0)
    {
        *transition_count += step;
        if (*transition_count >= BSP_ENCODER_TRANSITIONS_PER_DETENT)
        {
            *transition_count -= BSP_ENCODER_TRANSITIONS_PER_DETENT;
            emit_event(BSP_INPUT_ENCODER_CLOCKWISE, BSP_INPUT_INDEX_NONE);
        }
        else if (*transition_count <= -BSP_ENCODER_TRANSITIONS_PER_DETENT)
        {
            *transition_count += BSP_ENCODER_TRANSITIONS_PER_DETENT;
            emit_event(BSP_INPUT_ENCODER_COUNTER_CLOCKWISE, BSP_INPUT_INDEX_NONE);
        }
    }
    else if (current_phase != *previous_phase)
    {
        // 相位发生非法跨越时丢弃未完成卡点，防止产生错误方向事件。
        *transition_count = 0;
    }
    *previous_phase = current_phase;
}

// 扫描一个按键，并在消抖后的按下或释放边沿上报事件。
static void scan_button(struct button_state *state,
                        const char *name,
                        enum bsp_input_event pressed_event,
                        enum bsp_input_event released_event,
                        uint8_t index)
{
    if (update_button_state(state))
    {
        bool pressed = state->stable_level == 0U;

        ESP_LOGI(tag, "%s %s", name, pressed ? "pressed" : "released");
        emit_event(pressed ? pressed_event : released_event, index);
    }
}

// 扫描 KEY/POP 并仅上报单击、长按按下和长按松开动作。
static void scan_action_button(struct button_state *state,
                               const char *name,
                               uint8_t index)
{
    TickType_t now = xTaskGetTickCount();
    bool changed = update_button_state(state);
    bool pressed = state->stable_level == 0U;

    if (changed)
    {
        ESP_LOGI(tag, "%s %s", name, pressed ? "pressed" : "released");
        if (pressed)
        {
            state->pressed_tick = now;
            state->long_pressed = false;
            // Pop 键改为普通按键语义：轻按=单击（开/关应用菜单或返回），
            // 长按≥1s=长按（预留开关机）；不再按下即当长按。
        }
        else if (state->long_pressed)
        {
            emit_event(BSP_INPUT_LONG_RELEASED, index);
        }
        else
        {
            emit_event(BSP_INPUT_CLICK, index);
        }
    }

    if (pressed && !state->long_pressed &&
        (now - state->pressed_tick) >= pdMS_TO_TICKS(BSP_BUTTON_LONG_PRESS_MS))
    {
        state->long_pressed = true;
        emit_event(BSP_INPUT_LONG_PRESSED, index);
    }
}

// 以固定 1 ms 周期扫描旋钮、POP 和五个普通按键。
static void gpio_input_task(void *argument)
{
    struct button_state encoder_button;
    struct button_state pop_button;
    struct button_state key_buttons[BSP_KEY_COUNT];
    uint8_t previous_phase = read_encoder_phase();
    int8_t transition_count = 0;
    TickType_t wake_time = xTaskGetTickCount();

    (void)argument;
    init_button_state(&encoder_button, BSP_ENCODER_BUTTON_GPIO);
    init_button_state(&pop_button, BSP_POP_BUTTON_GPIO);
    for (uint8_t index = 0U; index < BSP_KEY_COUNT; index++)
    {
        init_button_state(&key_buttons[index], key_gpios[index]);
    }

    // xTaskDelayUntil 保持固定采样节拍，使毫秒去抖计数具有稳定时间基准。
    for (;;)
    {
        scan_encoder(&transition_count, &previous_phase);
        scan_button(&encoder_button, "Encoder button", BSP_INPUT_ENCODER_PRESSED,
                    BSP_INPUT_ENCODER_RELEASED, BSP_INPUT_INDEX_NONE);
        scan_action_button(&pop_button, "POP", BSP_INPUT_INDEX_POP);
        for (uint8_t index = 0U; index < BSP_KEY_COUNT; index++)
        {
            scan_action_button(&key_buttons[index], key_names[index], index);
        }

        xTaskDelayUntil(&wake_time, pdMS_TO_TICKS(BSP_INPUT_SAMPLE_PERIOD_MS));
    }
}

// 对指定摇杆 ADC 通道进行多次采样并返回算术平均值。
static esp_err_t read_adc_average(adc_channel_t channel, int *value)
{
    int raw = 0;
    int32_t sum = 0;

    for (uint32_t sample = 0U; sample < BSP_JOYSTICK_SAMPLE_COUNT; sample++)
    {
        ESP_RETURN_ON_ERROR(adc_oneshot_read(adc_handle, channel, &raw), tag,
                            "Read joystick ADC failed");
        sum += raw;
    }

    *value = (int)(sum / (int32_t)BSP_JOYSTICK_SAMPLE_COUNT);
    return ESP_OK;
}

// 将单轴 ADC 原始值按中心死区补偿后线性映射到 -100～100。
static int8_t map_joystick_axis(int raw, bool inverted)
{
    int offset = raw - BSP_JOYSTICK_CENTER;
    int magnitude = abs(offset);
    if (magnitude < BSP_JOYSTICK_DEAD_ZONE)
    {
        return 0;
    }

    int range = offset < 0 ?
                    BSP_JOYSTICK_CENTER - BSP_JOYSTICK_DEAD_ZONE -
                        BSP_JOYSTICK_RAW_MIN :
                    BSP_JOYSTICK_RAW_MAX - BSP_JOYSTICK_CENTER -
                        BSP_JOYSTICK_DEAD_ZONE;
    int mapped = ((magnitude - BSP_JOYSTICK_DEAD_ZONE) *
                  BSP_JOYSTICK_OUTPUT_MAX) / range;
    if (mapped < 1)
    {
        mapped = 1;
    }
    else if (mapped > BSP_JOYSTICK_OUTPUT_MAX)
    {
        mapped = BSP_JOYSTICK_OUTPUT_MAX;
    }
    int direction = offset < 0 ? -1 : 1;
    if (inverted)
    {
        direction = -direction;
    }
    return (int8_t)(mapped * direction);
}

// 将校准后的二维坐标转换为离线滚轮和测试页使用的单一主方向。
static enum bsp_joystick_state get_joystick_state(int8_t x, int8_t y)
{
    if (x == 0 && y == 0)
    {
        return BSP_JOYSTICK_NEUTRAL;
    }
    if (abs((int)x) >= abs((int)y))
    {
        // 两轴同时偏移时选择绝对偏移更大的轴，避免一次动作上报两个方向。
        return x < 0 ? BSP_JOYSTICK_LEFT : BSP_JOYSTICK_RIGHT;
    }
    return y < 0 ? BSP_JOYSTICK_UP : BSP_JOYSTICK_DOWN;
}

// 上报摇杆方向或回中变化，每次状态边沿只产生一个应用事件。
static void emit_joystick_state(enum bsp_joystick_state state, int x_raw, int y_raw)
{
    enum bsp_input_event event;
    const char *direction;

    switch (state)
    {
    case BSP_JOYSTICK_UP:
        event = BSP_INPUT_JOYSTICK_UP;
        direction = "UP";
        break;
    case BSP_JOYSTICK_DOWN:
        event = BSP_INPUT_JOYSTICK_DOWN;
        direction = "DOWN";
        break;
    case BSP_JOYSTICK_LEFT:
        event = BSP_INPUT_JOYSTICK_LEFT;
        direction = "LEFT";
        break;
    case BSP_JOYSTICK_RIGHT:
        event = BSP_INPUT_JOYSTICK_RIGHT;
        direction = "RIGHT";
        break;
    case BSP_JOYSTICK_NEUTRAL:
        ESP_LOGI(tag, "Joystick centered: x=%d, y=%d", x_raw, y_raw);
        event = BSP_INPUT_JOYSTICK_CENTER;
        direction = "CENTER";
        break;
    default:
        return;
    }

    if (state != BSP_JOYSTICK_NEUTRAL)
    {
        ESP_LOGI(tag, "Joystick outside dead zone: %s, x=%d, y=%d",
                 direction, x_raw, y_raw);
    }
    emit_event(event, BSP_INPUT_INDEX_NONE);
}

// 周期采样摇杆；非零坐标持续发布，回中发布一次 0,0 后停止。
static void joystick_task(void *argument)
{
    enum bsp_joystick_state previous_state = BSP_JOYSTICK_NEUTRAL;
    bool position_active = false;
    TickType_t wake_time = xTaskGetTickCount();

    (void)argument;
    for (;;)
    {
        int x_raw = 0;
        int y_raw = 0;
        esp_err_t x_result = read_adc_average(BSP_JOYSTICK_X_CHANNEL, &x_raw);
        esp_err_t y_result = read_adc_average(BSP_JOYSTICK_Y_CHANNEL, &y_raw);

        //ESP_LOGI(tag, "Joystick raw: x=%d, y=%d", x_raw, y_raw);

        if (x_result == ESP_OK && y_result == ESP_OK)
        {
            // 实物 X 轴 ADC 增大对应向左，Y 轴 ADC 减小对应向上。
            int8_t x = map_joystick_axis(x_raw, true);
            int8_t y = map_joystick_axis(y_raw, false);
            bool active = x != 0 || y != 0;
            if (active || position_active)
            {
                emit_joystick_position(x, y);
            }
            position_active = active;

            enum bsp_joystick_state state = get_joystick_state(x, y);
            if (state != previous_state)
            {
                // 状态边沿触发可避免摇杆保持偏移时高频重复上报和刷屏。
                emit_joystick_state(state, x_raw, y_raw);
                previous_state = state;
            }
        }

        xTaskDelayUntil(&wake_time, pdMS_TO_TICKS(BSP_JOYSTICK_SAMPLE_PERIOD_MS));
    }
}

// 配置所有低电平有效的按键和旋钮 GPIO 输入。
static esp_err_t init_input_gpio(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << BSP_ENCODER_BUTTON_GPIO) |
                        (1ULL << BSP_ENCODER_PHASE_A_GPIO) |
                        (1ULL << BSP_ENCODER_PHASE_B_GPIO) |
                        (1ULL << BSP_POP_BUTTON_GPIO) |
                        (1ULL << BSP_KEY_1_GPIO) |
                        (1ULL << BSP_KEY_2_GPIO) |
                        (1ULL << BSP_KEY_3_GPIO) |
                        (1ULL << BSP_KEY_4_GPIO) |
                        (1ULL << BSP_KEY_5_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&config);
}

// 创建 ADC1 单次采样单元并配置摇杆 X/Y 两个通道。
static esp_err_t init_joystick_adc(void)
{
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = BSP_JOYSTICK_ADC_UNIT,
    };
    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t result;

    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &adc_handle), tag,
                        "Create joystick ADC failed");
    result = adc_oneshot_config_channel(adc_handle, BSP_JOYSTICK_X_CHANNEL,
                                        &channel_config);
    if (result == ESP_OK)
    {
        result = adc_oneshot_config_channel(adc_handle, BSP_JOYSTICK_Y_CHANNEL,
                                            &channel_config);
    }
    if (result != ESP_OK)
    {
        // 任一通道配置失败都释放 ADC 单元，避免保留不可用的半初始化状态。
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }
    return result;
}

// 注册应用回调，初始化 GPIO/ADC，并创建 GPIO 与摇杆采样任务。
esp_err_t bsp_input_init(const struct bsp_config *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && config->input_callback != NULL &&
                            config->joystick_callback != NULL,
                        ESP_ERR_INVALID_ARG, tag, "Input callbacks are required");
    ESP_RETURN_ON_FALSE(gpio_input_task_handle == NULL && joystick_task_handle == NULL,
                        ESP_ERR_INVALID_STATE, tag, "Input is already initialized");

    input_callback = config->input_callback;
    input_context = config->input_context;
    joystick_callback = config->joystick_callback;
    joystick_context = config->joystick_context;
    ESP_RETURN_ON_ERROR(init_input_gpio(), tag, "Configure input GPIO failed");
    ESP_RETURN_ON_ERROR(init_joystick_adc(), tag, "Configure joystick failed");

    if (xTaskCreate(gpio_input_task, "gpio_input", BSP_GPIO_INPUT_TASK_STACK_SIZE, NULL,
                    CONFIG_BSP_INPUT_TASK_PRIORITY, &gpio_input_task_handle) != pdPASS)
    {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(joystick_task, "joystick_input", BSP_JOYSTICK_TASK_STACK_SIZE, NULL,
                    CONFIG_BSP_INPUT_TASK_PRIORITY, &joystick_task_handle) != pdPASS)
    {
        // 第二个任务创建失败时撤销先前任务和 ADC，保持初始化结果原子化。
        vTaskDelete(gpio_input_task_handle);
        gpio_input_task_handle = NULL;
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(tag, "Encoder (%u detents/rev), keys and joystick initialized",
             BSP_ENCODER_DETENTS_PER_REVOLUTION);
    return ESP_OK;
}
