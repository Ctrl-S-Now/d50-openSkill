/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 文件：bsp_amic.c
 * 描述：通过 ESP32-P4 ADC2 连续采样独立 AMIC 模拟链路，并转换为单声道 PCM。
 */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_adc/adc_continuous.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include "bsp.h"
#include "bsp_config.h"
#include "bsp_internal.h"

// 每个 UAC 10 ms 音频帧包含 160 个采样点，ADC DMA 每个结果占 4 字节。
#define AMIC_ADC_FRAME_SAMPLE_COUNT            (160U)
#define AMIC_ADC_FRAME_SIZE                    (AMIC_ADC_FRAME_SAMPLE_COUNT * SOC_ADC_DIGI_RESULT_BYTES)
#define AMIC_ADC_STORE_BUFFER_SIZE             (AMIC_ADC_FRAME_SIZE * 4U)
#define AMIC_ADC_READ_SAMPLE_COUNT             (64U)
#define AMIC_ADC_READ_BUFFER_SIZE              (AMIC_ADC_READ_SAMPLE_COUNT * SOC_ADC_DIGI_RESULT_BYTES)
#define AMIC_ADC_READ_DEADLINE_US              (10000LL)
#define AMIC_ADC_CALIBRATION_TIMEOUT_US        (1000000LL)
#define AMIC_ADC_SETTLE_SAMPLE_COUNT           (512U)
#define AMIC_ADC_CALIBRATION_SAMPLE_COUNT      (1024U)
#define AMIC_ADC_DC_MIN                        (64U)
#define AMIC_ADC_DC_MAX                        (4095U - AMIC_ADC_DC_MIN)
#define AMIC_ADC_CLIP_MARGIN                   (8U)
#define AMIC_ADC_PCM_SCALE                     (16)
#define AMIC_DIAG_INTERVAL_US                  (1000000LL)

static const char *tag = "bsp_amic";
static adc_continuous_handle_t adc_handle;
static bool adc_started;
static bool amic_initialized;
static uint32_t amic_dc_value;
static int64_t diag_last_log_us;
static uint32_t diag_raw_min = UINT32_MAX;
static uint32_t diag_raw_max;
static uint32_t diag_sample_count;
static uint32_t diag_clip_count;
static uint32_t diag_timeout_count;
static uint32_t diag_invalid_count;
static uint32_t diag_short_read_count;
static uint16_t diag_pcm_peak;

// 将微秒截止时间向上取整成 ADC 驱动所需的毫秒超时。
static uint32_t _remaining_timeout_ms(int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();

    if (remaining_us <= 0)
    {
        return 0U;
    }
    return (uint32_t)((remaining_us + 999LL) / 1000LL);
}

// 读取并解析最多 64 个 ADC DMA 结果，避免在高频路径动态分配内存。
static esp_err_t _read_adc_results(adc_continuous_data_t *results,
                                   uint32_t requested_samples,
                                   uint32_t timeout_ms,
                                   uint32_t *result_count)
{
    uint8_t raw_buffer[AMIC_ADC_READ_BUFFER_SIZE];
    uint32_t raw_size = 0U;
    uint32_t read_size;

    if (requested_samples > AMIC_ADC_READ_SAMPLE_COUNT)
    {
        requested_samples = AMIC_ADC_READ_SAMPLE_COUNT;
    }
    read_size = requested_samples * SOC_ADC_DIGI_RESULT_BYTES;
    *result_count = 0U;

    esp_err_t ret = adc_continuous_read(adc_handle, raw_buffer, read_size,
                                        &raw_size, timeout_ms);
    if (ret != ESP_OK)
    {
        return ret;
    }
    return adc_continuous_parse_data(adc_handle, raw_buffer, raw_size,
                                     results, result_count);
}

// 判断解析结果是否属于主板 AMIC 所在的 ADC2 通道。
static bool _is_amic_result(const adc_continuous_data_t *result)
{
    return result->valid &&
           result->unit == BSP_AMIC_ADC_UNIT &&
           result->channel == BSP_AMIC_ADC_CHANNEL;
}

// 丢弃上电暂态后累计固定数量样本，得到当前板卡的 ADC 直流中心。
static esp_err_t _calibrate_dc(void)
{
    adc_continuous_data_t results[AMIC_ADC_READ_SAMPLE_COUNT];
    uint64_t dc_sum = 0U;
    uint32_t discarded_count = 0U;
    uint32_t calibration_count = 0U;
    uint32_t invalid_count = 0U;
    int64_t deadline_us = esp_timer_get_time() + AMIC_ADC_CALIBRATION_TIMEOUT_US;

    while (calibration_count < AMIC_ADC_CALIBRATION_SAMPLE_COUNT)
    {
        uint32_t timeout_ms = _remaining_timeout_ms(deadline_us);
        uint32_t result_count = 0U;

        if (timeout_ms == 0U)
        {
            return ESP_ERR_TIMEOUT;
        }

        esp_err_t ret = _read_adc_results(results, AMIC_ADC_READ_SAMPLE_COUNT,
                                          timeout_ms, &result_count);
        if (ret == ESP_ERR_TIMEOUT)
        {
            continue;
        }
        if (ret != ESP_OK)
        {
            return ret;
        }

        for (uint32_t index = 0U; index < result_count; index++)
        {
            if (!_is_amic_result(&results[index]))
            {
                invalid_count++;
                continue;
            }
            if (discarded_count < AMIC_ADC_SETTLE_SAMPLE_COUNT)
            {
                discarded_count++;
                continue;
            }

            dc_sum += results[index].raw_data;
            calibration_count++;
            if (calibration_count >= AMIC_ADC_CALIBRATION_SAMPLE_COUNT)
            {
                break;
            }
        }
    }

    amic_dc_value = (uint32_t)(dc_sum / calibration_count);
    ESP_LOGI(tag, "Calibration: samples=%lu, discarded=%lu, invalid=%lu, dc=%lu",
             (unsigned long)calibration_count,
             (unsigned long)discarded_count,
             (unsigned long)invalid_count,
             (unsigned long)amic_dc_value);

    if (amic_dc_value < AMIC_ADC_DC_MIN || amic_dc_value > AMIC_ADC_DC_MAX)
    {
        ESP_LOGE(tag, "Invalid ADC bias: dc=%lu, check TP75 and C70",
                 (unsigned long)amic_dc_value);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

// 把 ADC 原始值减去实测直流中心，并按 12 位到 16 位的比例转换和限幅。
static int16_t _convert_to_pcm(uint32_t raw_value)
{
    int32_t pcm_value = ((int32_t)raw_value - (int32_t)amic_dc_value) *
                        AMIC_ADC_PCM_SCALE;

    if (pcm_value > INT16_MAX)
    {
        pcm_value = INT16_MAX;
    }
    else if (pcm_value < INT16_MIN)
    {
        pcm_value = INT16_MIN;
    }
    return (int16_t)pcm_value;
}

// 累计原始 ADC 与 PCM 指标，每秒限频输出一次诊断结果。
static void _update_diagnostic(uint32_t raw_value, int16_t pcm_value)
{
    int32_t magnitude = pcm_value;

    if (raw_value < diag_raw_min)
    {
        diag_raw_min = raw_value;
    }
    if (raw_value > diag_raw_max)
    {
        diag_raw_max = raw_value;
    }
    if (raw_value <= AMIC_ADC_CLIP_MARGIN ||
        raw_value >= (4095U - AMIC_ADC_CLIP_MARGIN))
    {
        diag_clip_count++;
    }
    if (magnitude < 0)
    {
        magnitude = -magnitude;
    }
    if ((uint32_t)magnitude > diag_pcm_peak)
    {
        diag_pcm_peak = (uint16_t)magnitude;
    }
    diag_sample_count++;
}

static void _log_diagnostic(void)
{
    int64_t now_us = esp_timer_get_time();

    if (diag_last_log_us == 0)
    {
        diag_last_log_us = now_us;
    }
    if (now_us - diag_last_log_us < AMIC_DIAG_INTERVAL_US)
    {
        return;
    }

    ESP_LOGI(tag,
             "DIAG: dc=%lu, samples=%lu, raw_min=%lu, raw_max=%lu, peak=%u, clip=%lu, timeout=%lu, invalid=%lu, short=%lu",
             (unsigned long)amic_dc_value,
             (unsigned long)diag_sample_count,
             (unsigned long)(diag_raw_min == UINT32_MAX ? 0U : diag_raw_min),
             (unsigned long)diag_raw_max,
             (unsigned int)diag_pcm_peak,
             (unsigned long)diag_clip_count,
             (unsigned long)diag_timeout_count,
             (unsigned long)diag_invalid_count,
             (unsigned long)diag_short_read_count);

    diag_last_log_us = now_us;
    diag_raw_min = UINT32_MAX;
    diag_raw_max = 0U;
    diag_sample_count = 0U;
    diag_clip_count = 0U;
    diag_timeout_count = 0U;
    diag_invalid_count = 0U;
    diag_short_read_count = 0U;
    diag_pcm_peak = 0U;
}

/*
 * 初始化 ADC2 连续采样并完成直流中心校准。
 * 任一步骤失败都会释放已创建的 ADC 资源。
 */
esp_err_t bsp_amic_init(void)
{
    if (amic_initialized)
    {
        return ESP_OK;
    }

    adc_continuous_handle_cfg_t handle_config = {
        .max_store_buf_size = AMIC_ADC_STORE_BUFFER_SIZE,
        .conv_frame_size = AMIC_ADC_FRAME_SIZE,
        .flags.flush_pool = 1,
    };
    adc_digi_pattern_config_t pattern = {
        .atten = ADC_ATTEN_DB_12,
        .channel = BSP_AMIC_ADC_CHANNEL,
        .unit = BSP_AMIC_ADC_UNIT,
        .bit_width = ADC_BITWIDTH_12,
    };
    adc_continuous_config_t adc_config = {
        .pattern_num = 1U,
        .adc_pattern = &pattern,
        .sample_freq_hz = BSP_AUDIO_SAMPLE_RATE_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_2,
    };
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_ERROR(adc_continuous_new_handle(&handle_config, &adc_handle),
                      error, tag, "Create ADC continuous handle failed");
    ESP_GOTO_ON_ERROR(adc_continuous_config(adc_handle, &adc_config),
                      error, tag, "Configure AMIC ADC failed");
    ESP_GOTO_ON_ERROR(adc_continuous_start(adc_handle),
                      error, tag, "Start AMIC ADC failed");
    adc_started = true;
    ESP_GOTO_ON_ERROR(_calibrate_dc(), error, tag, "Calibrate AMIC ADC failed");

    amic_initialized = true;
    ESP_LOGI(tag, "Initialized: GPIO%u, ADC2_CH5, %u Hz, dc=%lu, pcm_scale=x%d",
             (unsigned int)BSP_AMIC_ADC_GPIO,
             (unsigned int)BSP_AUDIO_SAMPLE_RATE_HZ,
             (unsigned long)amic_dc_value,
             AMIC_ADC_PCM_SCALE);
    return ESP_OK;

error:
    bsp_amic_deinit();
    return ret;
}

// 幂等释放 AMIC ADC continuous 资源。
void bsp_amic_deinit(void)
{
    if (adc_handle != NULL && adc_started)
    {
        esp_err_t ret = adc_continuous_stop(adc_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGW(tag, "Stop AMIC ADC failed: %s", esp_err_to_name(ret));
        }
    }
    adc_started = false;

    if (adc_handle != NULL)
    {
        esp_err_t ret = adc_continuous_deinit(adc_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGW(tag, "Delete AMIC ADC failed: %s", esp_err_to_name(ret));
        }
        adc_handle = NULL;
    }
    amic_initialized = false;
    amic_dc_value = 0U;
}

// 从 AMIC ADC 读取数据，并转换为 16 位小端单声道 PCM。
esp_err_t bsp_amic_read(void *buffer, size_t size, size_t *read_size)
{
    if (!amic_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (buffer == NULL || read_size == NULL || size == 0U ||
        (size % BSP_AUDIO_BYTES_PER_SAMPLE) != 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *pcm_buffer = buffer;
    size_t requested_samples = size / BSP_AUDIO_BYTES_PER_SAMPLE;
    size_t written_samples = 0U;
    int64_t deadline_us = esp_timer_get_time() + AMIC_ADC_READ_DEADLINE_US;

    *read_size = 0U;
    while (written_samples < requested_samples)
    {
        adc_continuous_data_t results[AMIC_ADC_READ_SAMPLE_COUNT];
        uint32_t remaining_samples = (uint32_t)(requested_samples - written_samples);
        uint32_t read_samples = remaining_samples < AMIC_ADC_READ_SAMPLE_COUNT ?
                                remaining_samples : AMIC_ADC_READ_SAMPLE_COUNT;
        uint32_t timeout_ms = _remaining_timeout_ms(deadline_us);
        uint32_t result_count = 0U;

        if (timeout_ms == 0U)
        {
            diag_timeout_count++;
            break;
        }

        esp_err_t ret = _read_adc_results(results, read_samples, timeout_ms,
                                          &result_count);
        if (ret == ESP_ERR_TIMEOUT)
        {
            diag_timeout_count++;
            break;
        }
        if (ret != ESP_OK)
        {
            _log_diagnostic();
            return ret;
        }

        for (uint32_t index = 0U; index < result_count &&
             written_samples < requested_samples; index++)
        {
            if (!_is_amic_result(&results[index]))
            {
                diag_invalid_count++;
                continue;
            }

            int16_t pcm_value = _convert_to_pcm(results[index].raw_data);
            uint16_t pcm_bits = (uint16_t)pcm_value;
            size_t pcm_offset = written_samples * BSP_AUDIO_BYTES_PER_SAMPLE;

            pcm_buffer[pcm_offset] = (uint8_t)(pcm_bits & 0xFFU);
            pcm_buffer[pcm_offset + 1U] = (uint8_t)(pcm_bits >> 8U);
            written_samples++;
            _update_diagnostic(results[index].raw_data, pcm_value);
        }
    }

    *read_size = written_samples * BSP_AUDIO_BYTES_PER_SAMPLE;
    if (*read_size < size)
    {
        diag_short_read_count++;
    }
    _log_diagnostic();
    return ESP_OK;
}
