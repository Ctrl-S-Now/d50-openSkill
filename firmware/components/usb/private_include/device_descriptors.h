/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 文件：device_descriptors.h
 * 描述：USB 复合设备描述符内部声明和接口编号定义。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

// 复合设备接口顺序必须与全速和高速配置描述符保持一致。
enum
{
    USB_ITF_CDC_CONTROL = 0,
    USB_ITF_CDC_DATA,
    USB_ITF_HID,
    USB_ITF_VENDOR_HID,
    USB_ITF_AUDIO_CONTROL,
    USB_ITF_AUDIO_STREAMING_MIC,
    USB_ITF_TOTAL,
};

// TinyUSB HID instance 0 为键鼠接口，instance 1 为 Vendor HID 接口。
enum
{
    USB_HID_REPORT_ID_KEYBOARD = 1,
    USB_HID_REPORT_ID_MOUSE,
    USB_HID_INSTANCE_KEYBOARD = 0,
    USB_HID_INSTANCE_VENDOR,
};

// 由 usb_descriptors.c 定义，并在 TinyUSB 初始化时统一注册。
extern const tusb_desc_device_t usb_device_descriptor;
extern const tusb_desc_device_qualifier_t usb_device_qualifier;
extern const uint8_t usb_full_speed_configuration_descriptor[];
extern const uint8_t usb_high_speed_configuration_descriptor[];
extern const char *usb_string_descriptors[];
extern const size_t usb_string_descriptor_count;
// 12 位十六进制基础 MAC 字符串，初始化 USB 前动态生成。
extern char usb_serial_string[13];
