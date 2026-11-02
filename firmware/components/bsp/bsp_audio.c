/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 文件：bsp_audio.c
 * 描述：配置 ES8311、I2C 和全双工 I2S，提供麦克风采集与扬声器输出接口。
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

#include "bsp.h"
#include "bsp_config.h"
#include "bsp_internal.h"

// ES8311 固定增益、音量和阻塞式 I2S 读写超时。
#define ES8311_MIC_GAIN_DB                      (42.0F)
#define ES8311_SPEAKER_VOLUME                   (100)
#define ES8311_IO_TIMEOUT_MS                    (100U)
static const char *tag = "bsp_audio";
// 音频硬件和 codec 接口句柄，初始化失败时按依赖逆序释放。
static i2c_master_bus_handle_t i2c_bus_handle;
static i2s_chan_handle_t i2s_tx_handle;
static i2s_chan_handle_t i2s_rx_handle;
static const audio_codec_data_if_t *codec_data_if;
static const audio_codec_ctrl_if_t *codec_ctrl_if;
static const audio_codec_gpio_if_t *codec_gpio_if;
static const audio_codec_if_t *codec_if;
static esp_codec_dev_handle_t codec_dev_handle;
// codec 打开且采样参数配置完成后置位。
static bool codec_initialized;

// 释放已创建的音频资源，可安全处理部分初始化状态。
static void _cleanup(void)
{
#if BSP_AUDIO_USE_AMIC_INPUT
    bsp_amic_deinit();
#endif

    // 先删除使用控制与数据接口的 codec 对象，再释放底层总线资源。
    if (codec_dev_handle != NULL)
    {
        esp_codec_dev_delete(codec_dev_handle);
        codec_dev_handle = NULL;
    }
    if (codec_if != NULL)
    {
        audio_codec_delete_codec_if(codec_if);
        codec_if = NULL;
    }
    if (codec_ctrl_if != NULL)
    {
        audio_codec_delete_ctrl_if(codec_ctrl_if);
        codec_ctrl_if = NULL;
    }
    if (codec_gpio_if != NULL)
    {
        audio_codec_delete_gpio_if(codec_gpio_if);
        codec_gpio_if = NULL;
    }
    if (codec_data_if != NULL)
    {
        audio_codec_delete_data_if(codec_data_if);
        codec_data_if = NULL;
    }
    if (i2s_tx_handle != NULL)
    {
        (void)i2s_channel_disable(i2s_tx_handle);
        (void)i2s_del_channel(i2s_tx_handle);
        i2s_tx_handle = NULL;
    }
    if (i2s_rx_handle != NULL)
    {
        (void)i2s_channel_disable(i2s_rx_handle);
        (void)i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = NULL;
    }
    if (i2c_bus_handle != NULL)
    {
        (void)i2c_del_master_bus(i2c_bus_handle);
        i2c_bus_handle = NULL;
    }
    codec_initialized = false;
}

/*
 * 初始化音频总线和 ES8311 编解码器。
 * 任一步骤失败均跳转到统一清理路径，不保留半初始化句柄。
 */
esp_err_t bsp_audio_init(void)
{
    esp_err_t ret = ESP_OK;
    i2c_master_bus_config_t i2c_config = {
        .i2c_port = BSP_AUDIO_I2C_PORT,
        .sda_io_num = BSP_AUDIO_I2C_SDA_GPIO,
        .scl_io_num = BSP_AUDIO_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(BSP_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    // 没有新数据时发送静音，否则 DMA 会一直重复最后一段数据（表现为提示音响个不停）。
    channel_config.auto_clear_after_cb = true;
    i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK_GPIO,
            .bclk = BSP_AUDIO_I2S_BCLK_GPIO,
            .ws = BSP_AUDIO_I2S_WS_GPIO,
            .dout = BSP_AUDIO_I2S_DOUT_GPIO,
            .din = BSP_AUDIO_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    if (codec_initialized)
    {
        return ESP_OK;
    }

    // 先建立 I2C/I2S 物理通路，再创建 codec 的数据、控制和 GPIO 抽象接口。
    ESP_GOTO_ON_ERROR(i2c_new_master_bus(&i2c_config, &i2c_bus_handle), error, tag,
                      "Create I2C bus failed");
    ESP_GOTO_ON_ERROR(i2s_new_channel(&channel_config, &i2s_tx_handle, &i2s_rx_handle), error, tag,
                      "Create I2S channels failed");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_tx_handle, &i2s_config), error, tag,
                      "Initialize I2S TX failed");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_rx_handle, &i2s_config), error, tag,
                      "Initialize I2S RX failed");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_tx_handle), error, tag, "Enable I2S TX failed");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_rx_handle), error, tag, "Enable I2S RX failed");

    audio_codec_i2s_cfg_t codec_i2s_config = {
        .port = BSP_AUDIO_I2S_PORT,
        .tx_handle = i2s_tx_handle,
        .rx_handle = i2s_rx_handle,
    };
    codec_data_if = audio_codec_new_i2s_data(&codec_i2s_config);
    ESP_GOTO_ON_FALSE(codec_data_if != NULL, ESP_ERR_NO_MEM, error, tag,
                      "Create codec I2S interface failed");

    audio_codec_i2c_cfg_t codec_i2c_config = {
        .port = BSP_AUDIO_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle,
    };
    codec_ctrl_if = audio_codec_new_i2c_ctrl(&codec_i2c_config);
    ESP_GOTO_ON_FALSE(codec_ctrl_if != NULL, ESP_ERR_NO_MEM, error, tag,
                      "Create codec I2C interface failed");

    codec_gpio_if = audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(codec_gpio_if != NULL, ESP_ERR_NO_MEM, error, tag,
                      "Create codec GPIO interface failed");

    esp_codec_dev_hw_gain_t hardware_gain = {
        .pa_voltage = 5.0F,
        .codec_dac_voltage = 3.3F,
    };
    es8311_codec_cfg_t es8311_config = {
        .ctrl_if = codec_ctrl_if,
        .gpio_if = codec_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_AUDIO_PA_ENABLE_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hardware_gain,
    };
    codec_if = es8311_codec_new(&es8311_config);
    ESP_GOTO_ON_FALSE(codec_if != NULL, ESP_ERR_NO_MEM, error, tag, "Create ES8311 codec failed");

    esp_codec_dev_cfg_t codec_device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = codec_data_if,
    };
    codec_dev_handle = esp_codec_dev_new(&codec_device_config);
    ESP_GOTO_ON_FALSE(codec_dev_handle != NULL, ESP_ERR_NO_MEM, error, tag,
                      "Create codec device failed");

    // codec 采样格式必须与 BSP 公共音频宏以及 USB UAC 格式保持一致。
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = BSP_AUDIO_SAMPLE_RATE_HZ,
        .channel = BSP_AUDIO_CHANNEL_COUNT,
        .bits_per_sample = BSP_AUDIO_BITS_PER_SAMPLE,
    };
    ESP_GOTO_ON_ERROR(esp_codec_dev_open(codec_dev_handle, &sample_info), error, tag,
                      "Open ES8311 codec failed");
    ESP_GOTO_ON_ERROR(esp_codec_dev_set_in_gain(codec_dev_handle, ES8311_MIC_GAIN_DB), error, tag,
                      "Set microphone gain failed");
    ESP_GOTO_ON_ERROR(esp_codec_dev_set_out_vol(codec_dev_handle, ES8311_SPEAKER_VOLUME), error, tag,
                      "Set speaker volume failed");

#if BSP_AUDIO_USE_AMIC_INPUT
    ESP_GOTO_ON_ERROR(bsp_amic_init(), error, tag, "Initialize AMIC input failed");
#endif

    codec_initialized = true;
#if BSP_AUDIO_USE_AMIC_INPUT
    ESP_LOGI(tag, "Initialized: %u Hz, %u-bit mono, input=AMIC ADC, speaker volume %d%%",
             BSP_AUDIO_SAMPLE_RATE_HZ, BSP_AUDIO_BITS_PER_SAMPLE,
             ES8311_SPEAKER_VOLUME);
#else
    ESP_LOGI(tag,
             "Initialized: %u Hz, %u-bit mono, input=ES8311 I2S, mic gain %.0f dB, speaker volume %d%%",
             BSP_AUDIO_SAMPLE_RATE_HZ, BSP_AUDIO_BITS_PER_SAMPLE,
             (double)ES8311_MIC_GAIN_DB, ES8311_SPEAKER_VOLUME);
#endif
    return ESP_OK;

error:
    // 清理函数覆盖所有部分初始化组合。
    _cleanup();
    return ret;
}

// 从板载麦克风读取对齐到完整采样点的单声道 PCM 数据。
esp_err_t bsp_audio_read(void *buffer, size_t size, size_t *read_size)
{
    if (!codec_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (buffer == NULL || read_size == NULL || size == 0U ||
        (size % BSP_AUDIO_BYTES_PER_SAMPLE) != 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *read_size = 0U;
#if BSP_AUDIO_USE_AMIC_INPUT
    return bsp_amic_read(buffer, size, read_size);
#else
    return i2s_channel_read(i2s_rx_handle, buffer, size, read_size, ES8311_IO_TIMEOUT_MS);
#endif
}

// 向扬声器 I2S 通路写入对齐到完整采样点的单声道 PCM 数据。
esp_err_t bsp_audio_write(const void *buffer, size_t size, size_t *write_size)
{
    if (!codec_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (buffer == NULL || write_size == NULL || size == 0U ||
        (size % BSP_AUDIO_BYTES_PER_SAMPLE) != 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *write_size = 0U;
    return i2s_channel_write(i2s_tx_handle, buffer, size, write_size, ES8311_IO_TIMEOUT_MS);
}

// 按照先静音再关闭、先开启再解除静音的顺序控制外部功放。
esp_err_t bsp_audio_set_speaker_enabled(bool enabled)
{
    if (!codec_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!enabled)
    {
        int codec_ret = esp_codec_dev_set_out_mute(codec_dev_handle, true);
        esp_err_t gpio_ret = gpio_set_level(BSP_AUDIO_PA_ENABLE_GPIO, 0);
        if (codec_ret != ESP_CODEC_DEV_OK)
        {
            return ESP_FAIL;
        }
        return gpio_ret;
    }

    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_AUDIO_PA_ENABLE_GPIO, 1), tag,
                        "Enable speaker PA failed");
    if (esp_codec_dev_set_out_mute(codec_dev_handle, false) != ESP_CODEC_DEV_OK)
    {
        (void)gpio_set_level(BSP_AUDIO_PA_ENABLE_GPIO, 0);
        return ESP_FAIL;
    }
    return ESP_OK;
}
