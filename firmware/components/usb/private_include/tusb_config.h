/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 文件：tusb_config.h
 * 描述：在 esp_tinyusb 目标配置上叠加 usb_device_uac 音频类配置。
 * 约束：构建系统将本包装头优先提供给三个 USB 目标，运行期初始化仍由 esp_tinyusb 负责。
 */

#pragma once

#include_next "tusb_config.h"
#include "tusb_config_uac.h"

/*
 * usb_device_uac 在 AS_PART 模式下仍会编译旧版 tusb_init() 兼容分支，
 * 这里固定高速端口 1 的设备模式，实际运行期初始化仍归 esp_tinyusb 所有。
 */
#ifndef CFG_TUSB_RHPORT1_MODE
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#endif

// 编译期校验复合设备类数量和 UAC 单向麦克风拓扑。
#if CFG_TUD_CDC != 1
#error "USB composite requires exactly one CDC interface"
#endif

#if CFG_TUD_HID != 2
#error "USB composite requires keyboard and vendor HID interfaces"
#endif

#if CFG_TUD_AUDIO != 1
#error "USB composite requires exactly one UAC function"
#endif

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX != 0
#error "USB composite must not expose a USB speaker"
#endif

#if CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX != 1
#error "USB composite requires one UAC microphone channel"
#endif
