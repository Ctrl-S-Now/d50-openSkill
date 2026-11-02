/*
 * 文件：bsp.c
 * 描述：D50 板级支持包初始化入口，统一启动已确认的 P0 板载外设。
 */

#include "bsp.h"

#include "bsp_internal.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *tag = "bsp";
// BSP 完成一次初始化后保持幂等，避免重复创建驱动资源和任务。
static bool initialized;

// 按 EEPROM、显示、两组 RGB 灯、音频、输入的顺序初始化主板外设。
esp_err_t bsp_init(const struct bsp_config *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && config->input_callback != NULL &&
                            config->joystick_callback != NULL,
                         ESP_ERR_INVALID_ARG, tag, "Invalid board configuration");

    if (initialized)
    {
        return ESP_OK;
    }

    // 输入任务最后启动，避免回调触发时其他板级服务尚未就绪。
    esp_err_t eeprom_result = bsp_eeprom_init();
    if (eeprom_result != ESP_OK)
    {
        // EEPROM 不可用时上层仍可回退到 NVS，不能阻塞其余板级外设启动。
        ESP_LOGW(tag, "EEPROM unavailable: %s",
                 esp_err_to_name(eeprom_result));
    }
    ESP_RETURN_ON_ERROR(bsp_display_init(), tag, "Display initialization failed");
    ESP_RETURN_ON_ERROR(bsp_led_init(), tag, "LED initialization failed");
    ESP_RETURN_ON_ERROR(bsp_audio_init(), tag, "Audio initialization failed");
    ESP_RETURN_ON_ERROR(bsp_input_init(config),
                         tag, "Input initialization failed");

    initialized = true;
    ESP_LOGI(tag, "D50 board initialized");
    return ESP_OK;
}
