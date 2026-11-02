/*
 * 文件：bsp_internal.h
 * 描述：BSP 组件内部初始化接口，由 bsp.c 按依赖顺序统一调用。
 */

#pragma once

#include "bsp.h"

// 各驱动初始化函数仅供 BSP 组件内部使用。
esp_err_t bsp_display_init(void);
esp_err_t bsp_eeprom_init(void);
esp_err_t bsp_input_init(const struct bsp_config *config);
esp_err_t bsp_audio_init(void);
esp_err_t bsp_amic_init(void);
esp_err_t bsp_amic_read(void *buffer, size_t size, size_t *read_size);
void bsp_amic_deinit(void);
esp_err_t bsp_led_init(void);
