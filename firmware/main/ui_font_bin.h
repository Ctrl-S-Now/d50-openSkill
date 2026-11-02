/*
 * 文件：ui_font_bin.h
 * 描述：从只读内存映射数据创建按字解码的 LVGL BIN 字体。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/**
 * @brief 从 LVGL BIN 字体数据创建只读字体对象。
 * @param data 字体文件的内存映射首地址。
 * @param size 字体文件长度。
 * @return 成功返回字体对象，格式或内存错误返回 NULL。
 */
lv_font_t *ui_font_bin_create(const uint8_t *data, size_t size);

/**
 * @brief 销毁由 ui_font_bin_create() 创建的字体对象。
 * @param font 待销毁的字体对象，允许传入 NULL。
 */
void ui_font_bin_destroy(lv_font_t *font);
