/*
 * 文件：bsp_display.c
 * 描述：初始化 ST7701 MIPI-DSI 显示屏、背光和 LVGL 适配器。
 */

#include "bsp.h"

#include "bsp_config.h"
#include "bsp_internal.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"

// 已验证的 LCD 行场同步时序参数。
#define BSP_LCD_HSYNC_PULSE_WIDTH            (63U)
#define BSP_LCD_HSYNC_BACK_PORCH             (63U)
#define BSP_LCD_HSYNC_FRONT_PORCH            (63U)
#define BSP_LCD_VSYNC_PULSE_WIDTH            (4U)
#define BSP_LCD_VSYNC_BACK_PORCH             (15U)
#define BSP_LCD_VSYNC_FRONT_PORCH            (20U)

// 传递给 ST7701 创建函数的 DSI 总线和 DPI 时序配置。
struct bsp_panel_config
{
    esp_lcd_dsi_bus_handle_t dsi_bus;
    const esp_lcd_dpi_panel_config_t *dpi_config;
};

static const char *tag = "bsp_display";
// 显示资源按电源、总线、面板 IO、面板、LVGL 适配器的依赖关系持有。
static esp_ldo_channel_handle_t dsi_phy_power_handle;
static esp_lcd_dsi_bus_handle_t dsi_bus_handle;
static esp_lcd_panel_io_handle_t panel_io_handle;
static esp_lcd_panel_handle_t panel_handle;
static bool lvgl_adapter_initialized;

/*
 * 背光由电源使能（BL_EN）和 PWM 亮度（BL_PWM）共同控制。
 * PWM 用 LEDC 输出 5 kHz、10 位占空比；亮度 0…255 按 γ=2 映射，人眼观感才接近线性。
 * 亮度为 0 时同时关掉电源使能，避免漏光。
 */
#define BACKLIGHT_PWM_FREQ_HZ     (5000U)
#define BACKLIGHT_PWM_MAX_DUTY    (1023U)
#define BACKLIGHT_FADE_STEP_MS    (10U)

static bool backlight_pwm_ready;
static uint8_t backlight_level;

static esp_err_t backlight_pwm_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), tag, "Backlight PWM timer failed");
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BACKLIGHT_PWM_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), tag, "Backlight PWM channel failed");
    backlight_pwm_ready = true;
    return ESP_OK;
}

static esp_err_t apply_backlight_level(uint8_t level)
{
    if (!backlight_pwm_ready)
    {
        ESP_RETURN_ON_ERROR(backlight_pwm_init(), tag, "Backlight PWM init failed");
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_BACKLIGHT_ENABLE_GPIO, level > 0U ? 1 : 0), tag,
                        "Set backlight power failed");
    uint32_t duty = ((uint32_t)level * level * BACKLIGHT_PWM_MAX_DUTY) / (255U * 255U);
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), tag,
                        "Set backlight duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), tag,
                        "Update backlight duty failed");
    backlight_level = level;
    return ESP_OK;
}

static esp_err_t set_backlight_enabled(bool enabled)
{
    return apply_backlight_level(enabled ? 255U : 0U);
}

// ST7701 厂商初始化命令，来自已验证的屏幕配置，顺序和延时不可随意调整。
static const st7701_lcd_init_cmd_t init_commands[] = {
    {0x11, NULL, 0U, 120U},
    {0xFF, (const uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5U, 0U},
    {0xEF, (const uint8_t[]){0x08}, 1U, 0U},
    {0xFF, (const uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5U, 0U},
    {0xC0, (const uint8_t[]){0x4F, 0x00}, 2U, 0U},
    {0xC1, (const uint8_t[]){0x11, 0x02}, 2U, 0U},
    {0xC2, (const uint8_t[]){0x30, 0x10}, 2U, 0U},
    {0xCC, (const uint8_t[]){0x10}, 1U, 0U},
    {0xB0, (const uint8_t[]){0xC0, 0x0E, 0x58, 0x0E, 0x13, 0x07, 0x0E, 0x1C, 0x0B, 0x27, 0x06, 0x13, 0x10, 0x6C, 0x32, 0x93}, 16U, 0U},
    {0xB1, (const uint8_t[]){0xC0, 0x15, 0x5A, 0x0D, 0x90, 0x06, 0x02, 0x05, 0x05, 0x1D, 0x00, 0xCD, 0x0D, 0x63, 0xA9, 0xCB}, 16U, 0U},
    {0xFF, (const uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5U, 0U},
    {0xB0, (const uint8_t[]){0x2D}, 1U, 0U},
    {0xB1, (const uint8_t[]){0x73}, 1U, 0U},
    {0xB2, (const uint8_t[]){0x85}, 1U, 0U},
    {0xB3, (const uint8_t[]){0x80}, 1U, 0U},
    {0xB5, (const uint8_t[]){0x4E}, 1U, 0U},
    {0xB7, (const uint8_t[]){0x85}, 1U, 0U},
    {0xB8, (const uint8_t[]){0x33}, 1U, 0U},
    {0xB9, (const uint8_t[]){0x10, 0x1F}, 2U, 0U},
    {0xBB, (const uint8_t[]){0x03}, 1U, 0U},
    {0xBC, (const uint8_t[]){0x00}, 1U, 0U},
    {0xC1, (const uint8_t[]){0x78}, 1U, 0U},
    {0xC2, (const uint8_t[]){0x78}, 1U, 0U},
    {0xD0, (const uint8_t[]){0x88}, 1U, 0U},
    {0xE0, (const uint8_t[]){0x00, 0x00, 0x02}, 3U, 0U},
    {0xE1, (const uint8_t[]){0x04, 0xA0, 0x06, 0xA0, 0x04, 0xA0, 0x05, 0xA0, 0x00, 0x44, 0x44}, 11U, 0U},
    {0xE2, (const uint8_t[]){0x20, 0x20, 0x40, 0x40, 0x99, 0xA0, 0x00, 0xA0, 0x99, 0xA0, 0x00, 0xA0, 0x00}, 13U, 0U},
    {0xE3, (const uint8_t[]){0x00, 0x00, 0x22, 0x22}, 4U, 0U},
    {0xE4, (const uint8_t[]){0x44, 0x44}, 2U, 0U},
    {0xE5, (const uint8_t[]){0x0E, 0x96, 0xA0, 0xA0, 0x10, 0x98, 0xA0, 0xA0, 0x0A, 0x92, 0xA0, 0xA0, 0x0C, 0x94, 0xA0, 0xA0}, 16U, 0U},
    {0xE6, (const uint8_t[]){0x00, 0x00, 0x22, 0x22}, 4U, 0U},
    {0xE7, (const uint8_t[]){0x44, 0x44}, 2U, 0U},
    {0xE7, (const uint8_t[]){0x44, 0x44}, 2U, 0U},
    {0xE8, (const uint8_t[]){0x0D, 0x95, 0xA0, 0xA0, 0x0F, 0x97, 0xA0, 0xA0, 0x09, 0x91, 0xA0, 0xA0, 0x0B, 0x93, 0xA0, 0xA0}, 16U, 0U},
    {0xEB, (const uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7U, 0U},
    {0xEC, (const uint8_t[]){0x50, 0x01}, 2U, 0U},
    {0xED, (const uint8_t[]){0xFF, 0xB1, 0xBA, 0x29, 0x84, 0x56, 0x7F, 0xFF, 0xFF, 0xF7, 0x65, 0x48, 0x92, 0xAB, 0x1B, 0xFF}, 16U, 0U},
    {0xEF, (const uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6U, 0U},
    {0xFF, (const uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5U, 0U},
    // {0x36, (const uint8_t[]){0x60}, 1U, 20U},
    {0x29, NULL, 0U, 20U},
};

// 校验厂商扩展配置，并据此创建 MIPI 接口的 ST7701 面板实例。
static esp_err_t new_panel(esp_lcd_panel_io_handle_t io,
                           const esp_lcd_panel_dev_config_t *panel_dev_config,
                           esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io != NULL, ESP_ERR_INVALID_ARG, tag, "invalid panel IO");
    ESP_RETURN_ON_FALSE(panel_dev_config != NULL, ESP_ERR_INVALID_ARG, tag, "invalid panel config");
    ESP_RETURN_ON_FALSE(ret_panel != NULL, ESP_ERR_INVALID_ARG, tag, "invalid panel output");

    const struct bsp_panel_config *driver_config =
        (const struct bsp_panel_config *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(driver_config != NULL, ESP_ERR_INVALID_ARG, tag, "invalid vendor config");
    ESP_RETURN_ON_FALSE(driver_config->dsi_bus != NULL, ESP_ERR_INVALID_ARG, tag, "invalid DSI bus");
    ESP_RETURN_ON_FALSE(driver_config->dpi_config != NULL, ESP_ERR_INVALID_ARG, tag, "invalid DPI config");

    st7701_vendor_config_t vendor_config = {
        .init_cmds = init_commands,
        .init_cmds_size = sizeof(init_commands) / sizeof(init_commands[0]),
        .mipi_config = {
            .dsi_bus = driver_config->dsi_bus,
            .dpi_config = driver_config->dpi_config,
        },
        .flags.use_mipi_interface = 1,
    };
    esp_lcd_panel_dev_config_t st7701_panel_config = *panel_dev_config;
    st7701_panel_config.vendor_config = &vendor_config;

    return esp_lcd_new_panel_st7701(io, &st7701_panel_config, ret_panel);
}

// 根据固定分辨率和同步参数生成 DPI 视频时序配置。
static esp_lcd_dpi_panel_config_t get_dpi_config(void)
{
    esp_lcd_dpi_panel_config_t config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BSP_LCD_DPI_CLOCK_MHZ,
        .virtual_channel = 0,
        .num_fbs = BSP_LCD_FRAME_BUFFER_COUNT,
        .video_timing = {
            .h_size = BSP_LCD_H_RES,
            .v_size = BSP_LCD_V_RES,
            .hsync_back_porch = BSP_LCD_HSYNC_BACK_PORCH,
            .hsync_pulse_width = BSP_LCD_HSYNC_PULSE_WIDTH,
            .hsync_front_porch = BSP_LCD_HSYNC_FRONT_PORCH,
            .vsync_back_porch = BSP_LCD_VSYNC_BACK_PORCH,
            .vsync_pulse_width = BSP_LCD_VSYNC_PULSE_WIDTH,
            .vsync_front_porch = BSP_LCD_VSYNC_FRONT_PORCH,
        },
    };

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    // ESP-IDF 6.x 使用输入、输出颜色格式字段描述 RGB888 数据流。
    config.in_color_format = LCD_COLOR_FMT_RGB888;
    config.out_color_format = LCD_COLOR_FMT_RGB888;
#else
    config.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
    config.flags.use_dma2d = true;
#endif

    return config;
}

// 关闭背光并按初始化依赖的逆序释放显示资源。
static void display_cleanup(void)
{
    (void)set_backlight_enabled(false);
    if (lvgl_adapter_initialized)
    {
        esp_lv_adapter_deinit();
        lvgl_adapter_initialized = false;
    }
    if (panel_handle != NULL)
    {
        esp_lcd_panel_del(panel_handle);
        panel_handle = NULL;
    }
    if (panel_io_handle != NULL)
    {
        esp_lcd_panel_io_del(panel_io_handle);
        panel_io_handle = NULL;
    }
    if (dsi_bus_handle != NULL)
    {
        esp_lcd_del_dsi_bus(dsi_bus_handle);
        dsi_bus_handle = NULL;
    }
    if (dsi_phy_power_handle != NULL)
    {
        esp_ldo_release_channel(dsi_phy_power_handle);
        dsi_phy_power_handle = NULL;
    }
}


/*
 * 初始化背光 GPIO、DSI PHY、ST7701 面板和 LVGL 显示适配器。
 * 初始化完成后保持背光关闭，由应用刷新初始页面并延时后开启。
 */
esp_err_t bsp_display_init(void)
{
    esp_err_t ret = ESP_OK;
    lv_display_t *display = NULL;
    gpio_config_t backlight_config = {
        .pin_bit_mask = (1ULL << BSP_LCD_BACKLIGHT_PWM_GPIO) |
                        (1ULL << BSP_LCD_BACKLIGHT_ENABLE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_ldo_channel_config_t ldo_config = {
        .chan_id = BSP_LCD_DSI_LDO_CHANNEL,
        .voltage_mv = BSP_LCD_DSI_LDO_VOLTAGE_MV,
    };
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = BSP_LCD_DSI_LANE_COUNT,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = BSP_LCD_DSI_LANE_RATE_MBPS,
    };
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_dpi_panel_config_t dpi_config = get_dpi_config();
    struct bsp_panel_config vendor_config = {
        .dsi_bus = NULL,
        .dpi_config = &dpi_config,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_config,
        .flags.reset_active_high = false,
    };

    // 初始化开始即关闭背光，随后依次建立 DSI 电源、总线和面板对象。
    ESP_GOTO_ON_ERROR(gpio_config(&backlight_config), error, tag, "Configure backlight failed");
    ESP_GOTO_ON_ERROR(set_backlight_enabled(false), error, tag, "Disable backlight failed");
    ESP_GOTO_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &dsi_phy_power_handle), error, tag,
                      "Acquire DSI PHY power failed");
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus_handle), error, tag,
                      "Create DSI bus failed");
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus_handle, &dbi_config, &panel_io_handle),
                      error, tag, "Create panel IO failed");

    vendor_config.dsi_bus = dsi_bus_handle;
    ESP_GOTO_ON_ERROR(new_panel(panel_io_handle, &panel_config, &panel_handle), error, tag,
                      "Create ST7701 panel failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel_handle), error, tag, "Reset panel failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel_handle), error, tag, "Initialize panel failed");
    ESP_GOTO_ON_ERROR(esp_lcd_dpi_panel_set_pattern(panel_handle, MIPI_DSI_PATTERN_NONE),
                      error, tag, "Disable DSI test pattern failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true), error, tag,
                      "Enable panel failed");

    // 三缓冲局部刷新用于降低竖屏旋转场景下的撕裂风险。
    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
        panel_handle, panel_io_handle, BSP_LCD_H_RES, BSP_LCD_V_RES,
        ESP_LV_ADAPTER_ROTATE_90);
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;

    ESP_GOTO_ON_ERROR(esp_lv_adapter_init(&adapter_config), error, tag,
                      "Initialize LVGL adapter failed");
    lvgl_adapter_initialized = true;
    display = esp_lv_adapter_register_display(&display_config);
    ESP_GOTO_ON_FALSE(display != NULL, ESP_FAIL, error, tag, "Register LVGL display failed");
    ESP_GOTO_ON_ERROR(esp_lv_adapter_start(), error, tag, "Start LVGL adapter failed");

    ESP_LOGI(tag, "ST7701 display ready: %ux%u RGB888", BSP_LCD_H_RES,
             BSP_LCD_V_RES);
    return ESP_OK;

error:
    // 统一回收所有已成功创建的资源，并确保背光保持关闭。
    display_cleanup();
    return ret;
}

// 显示初始化成功后，由应用控制背光开启时机。
esp_err_t bsp_display_set_backlight(bool enabled)
{
    ESP_RETURN_ON_FALSE(lvgl_adapter_initialized, ESP_ERR_INVALID_STATE, tag,
                        "LVGL adapter is not initialized");
    return set_backlight_enabled(enabled);
}

esp_err_t bsp_display_set_brightness(uint8_t level)
{
    ESP_RETURN_ON_FALSE(lvgl_adapter_initialized, ESP_ERR_INVALID_STATE, tag,
                        "LVGL adapter is not initialized");
    return apply_backlight_level(level);
}

// 线性渐变到目标亮度；阻塞调用，约 duration_ms 后返回。
esp_err_t bsp_display_fade_brightness(uint8_t target, uint32_t duration_ms)
{
    ESP_RETURN_ON_FALSE(lvgl_adapter_initialized, ESP_ERR_INVALID_STATE, tag,
                        "LVGL adapter is not initialized");
    uint32_t steps = duration_ms / BACKLIGHT_FADE_STEP_MS;
    int start = backlight_level;
    for (uint32_t step = 1U; step <= steps; step++)
    {
        int level = start + ((int)target - start) * (int)step / (int)steps;
        ESP_RETURN_ON_ERROR(apply_backlight_level((uint8_t)level), tag, "Fade step failed");
        vTaskDelay(pdMS_TO_TICKS(BACKLIGHT_FADE_STEP_MS));
    }
    return apply_backlight_level(target);
}

// 获取 LVGL 适配器互斥锁，调用方完成对象操作后必须解锁。
esp_err_t bsp_display_lock(int timeout_ms)
{
    ESP_RETURN_ON_FALSE(lvgl_adapter_initialized, ESP_ERR_INVALID_STATE, tag,
                        "LVGL adapter is not initialized");
    return esp_lv_adapter_lock(timeout_ms);
}

// 释放 LVGL 适配器互斥锁；适配器未初始化时不执行操作。
void bsp_display_unlock(void)
{
    if (lvgl_adapter_initialized)
    {
        esp_lv_adapter_unlock();
    }
}
