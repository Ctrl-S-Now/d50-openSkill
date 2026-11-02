/*
 * sound.c — 播放内置的 16 kHz / 单声道 / 16 bit WAV 提示音。
 * 音频文件在 main/sounds/，通过 EMBED_FILES 编进固件，不占文件系统。
 */
#include "sound.h"

#include <stdbool.h>
#include <stdint.h>

#include "bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const uint8_t boot_wav_start[] asm("_binary_boot_wav_start");
extern const uint8_t boot_wav_end[] asm("_binary_boot_wav_end");
extern const uint8_t click_wav_start[] asm("_binary_click_wav_start");
extern const uint8_t click_wav_end[] asm("_binary_click_wav_end");

#define WAV_HEADER_BYTES   (44U)   // 标准 PCM WAV 头
#define CHUNK_BYTES        (1024U)

static const char *tag = "sound";
static volatile bool playing;

struct clip
{
    const uint8_t *data;
    size_t size;
    int gain_percent;   // 音量百分比，100 = 原样
};

static void play_task(void *argument)
{
    struct clip clip = *(struct clip *)argument;
    int16_t scaled[CHUNK_BYTES / sizeof(int16_t)];
    for (size_t offset = 0U; offset < clip.size; offset += CHUNK_BYTES)
    {
        size_t chunk = clip.size - offset < CHUNK_BYTES ? clip.size - offset : CHUNK_BYTES;
        const void *source = clip.data + offset;
        if (clip.gain_percent != 100)
        {
            const int16_t *input = (const int16_t *)source;   // WAV 数据 16 位对齐
            for (size_t index = 0U; index < chunk / sizeof(int16_t); index++)
            {
                scaled[index] = (int16_t)((int)input[index] * clip.gain_percent / 100);
            }
            source = scaled;
        }
        size_t written = 0U;
        if (bsp_audio_write(source, chunk, &written) != ESP_OK)
        {
            ESP_LOGW(tag, "audio write failed");
            break;
        }
    }
    playing = false;
    vTaskDelete(NULL);
}

static esp_err_t play(const uint8_t *start, const uint8_t *end, int gain_percent)
{
    static struct clip clip;
    if (playing)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = bsp_audio_set_speaker_enabled(true);
    if (result != ESP_OK)
    {
        return result;
    }
    clip.data = start + WAV_HEADER_BYTES;
    clip.size = (size_t)(end - start) - WAV_HEADER_BYTES;
    clip.size &= ~(size_t)1U;   // 16 bit 对齐
    clip.gain_percent = gain_percent;
    playing = true;
    if (xTaskCreate(play_task, "sound", 4096, &clip, 5, NULL) != pdPASS)
    {
        playing = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sound_play_boot(void)
{
    return play(boot_wav_start, boot_wav_end, 100);
}

esp_err_t sound_play_click(void)
{
    return play(click_wav_start, click_wav_end, 18);   // 点击音很小声，只是个提示
}
