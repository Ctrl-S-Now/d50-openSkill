/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 文件：usb_descriptors.c
 * 描述：定义 USB 2.0 高速 CDC、键盘鼠标 HID 和 UAC2 麦克风复合设备描述符。
 * 约束：全速与高速配置保持相同接口拓扑，仅数据包大小和轮询周期不同。
 */

#include <stddef.h>
#include <stdint.h>

#include "class/hid/hid_device.h"
#include "tusb.h"
#include "uac_descriptors.h"

#include "device_descriptors.h"
#include "usb_protocol.h"

// USB 设备标识，修改会影响主机侧设备识别和驱动缓存。
#define USB_VID                             (0x303AU)
#define USB_PID                             (0x4015U)
#define USB_BCD_VERSION                     (0x0201U)

// 各功能固定端点地址；IN 端点在文件末尾进行编译期冲突检查。
#define USB_EP_CDC_NOTIFICATION                 (0x81U)
#define USB_EP_CDC_OUT                          (0x02U)
#define USB_EP_CDC_IN                           (0x82U)
#define USB_EP_HID_IN                           (0x83U)
#define USB_EP_AUDIO_MIC_IN                     (0x84U)
#define USB_EP_VENDOR_HID_OUT                   (0x05U)
#define USB_EP_VENDOR_HID_IN                    (0x85U)

// CDC 数据包大小和 HID 在不同总线速度下的轮询参数。
#define USB_CDC_NOTIFICATION_SIZE               (8U)
#define USB_CDC_DATA_SIZE_FS                    (64U)
#define USB_CDC_DATA_SIZE_HS                    (512U)
#define USB_HID_POLL_INTERVAL_FS                (10U)
#define USB_HID_POLL_INTERVAL_HS                (7U)
#define USB_BOS_TOTAL_LENGTH                    (12U)
#define USB_BOS_CAPABILITY_COUNT                (1U)
#define USB_HID_INTERFACE_PROTOCOL              HID_ITF_PROTOCOL_NONE

// 字符串描述符索引，必须与 usb_string_descriptors 数组顺序一致。
enum
{
    USB_STR_LANGUAGE = 0,
    USB_STR_MANUFACTURER,
    USB_STR_PRODUCT,
    USB_STR_SERIAL,
    USB_STR_HID,
    USB_STR_VENDOR_HID,
    USB_STR_AUDIO_CONTROL,
    USB_STR_AUDIO_MIC,
};

// 配置描述符总长度由 CDC、HID 和 UAC 各功能描述符长度相加得到。
enum
{
    USB_CONFIGURATION_TOTAL_LENGTH = TUD_CONFIG_DESC_LEN +
                                          TUD_CDC_DESC_LEN +
                                          TUD_HID_DESC_LEN +
                                          TUD_HID_INOUT_DESC_LEN +
                                          TUD_AUDIO_DEVICE_DESC_LEN,
};

// 单一 HID 接口通过 Report ID 同时承载标准键盘和相对鼠标报告。
static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(USB_HID_REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(USB_HID_REPORT_ID_MOUSE))
};

// Vendor HID 使用无 Report ID 的 64 字节通用输入/输出报告。
static const uint8_t vendor_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(CFG_TUD_HID_EP_BUFSIZE),
};

// 最小 BOS 只声明 USB 2.0 Extension，bmAttributes=0 表示不声明 LPM。
static const uint8_t bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(USB_BOS_TOTAL_LENGTH, USB_BOS_CAPABILITY_COUNT),
    0x07,
    TUSB_DESC_DEVICE_CAPABILITY,
    DEVICE_CAPABILITY_USB20_EXTENSION,
    U32_TO_U8S_LE(0x00000000),
};

// 字符串 0 使用 USB 规定的美式英语语言标识；序列号在初始化时填充。
static const char language_descriptor[] = {0x09, 0x04};
char usb_serial_string[13];

const char *usb_string_descriptors[] = {
    language_descriptor,
    "ULANZI STUDIO",
    "D50 VibeBar",
    usb_serial_string,
    "D50 Keyboard & Mouse",
    "D50 Vendor Protocol",
    "D50 MIC",
    "D50 MIC",
};

const size_t usb_string_descriptor_count =
    sizeof(usb_string_descriptors) / sizeof(usb_string_descriptors[0]);

// 复合设备使用 IAD 类代码，使主机将多个接口识别为同一设备功能集合。
const tusb_desc_device_t usb_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD_VERSION,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = USB_STR_MANUFACTURER,
    .iProduct = USB_STR_PRODUCT,
    .iSerialNumber = USB_STR_SERIAL,
    .bNumConfigurations = 1,
};

// 高速设备在其他速度查询中返回的设备限定描述符。
const tusb_desc_device_qualifier_t usb_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = USB_BCD_VERSION,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

// USB Full-Speed 配置：CDC 64 字节数据包，HID 10 ms 轮询。
const uint8_t usb_full_speed_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1,
                          USB_ITF_TOTAL,
                          0,
                          USB_CONFIGURATION_TOTAL_LENGTH,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
                          100),

    TUD_CDC_DESCRIPTOR(USB_ITF_CDC_CONTROL,
                       0,
                       USB_EP_CDC_NOTIFICATION,
                       USB_CDC_NOTIFICATION_SIZE,
                       USB_EP_CDC_OUT,
                       USB_EP_CDC_IN,
                       USB_CDC_DATA_SIZE_FS),

    TUD_HID_DESCRIPTOR(USB_ITF_HID,
                       USB_STR_HID,
                       USB_HID_INTERFACE_PROTOCOL,
                       sizeof(hid_report_descriptor),
                       USB_EP_HID_IN,
                       CFG_TUD_HID_EP_BUFSIZE,
                       USB_HID_POLL_INTERVAL_FS),

    TUD_HID_INOUT_DESCRIPTOR(USB_ITF_VENDOR_HID,
                              USB_STR_VENDOR_HID,
                              false,
                              sizeof(vendor_hid_report_descriptor),
                              USB_EP_VENDOR_HID_OUT,
                              USB_EP_VENDOR_HID_IN,
                              USB_PROTOCOL_REPORT_SIZE,
                              USB_HID_POLL_INTERVAL_FS),

    TUD_AUDIO_DESCRIPTOR(USB_ITF_AUDIO_CONTROL,
                         USB_STR_AUDIO_CONTROL,
                         0,
                         USB_EP_AUDIO_MIC_IN,
                         0),
};

// USB High-Speed 配置：CDC 512 字节数据包，HID 使用高速帧轮询指数。
const uint8_t usb_high_speed_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1,
                          USB_ITF_TOTAL,
                          0,
                          USB_CONFIGURATION_TOTAL_LENGTH,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
                          100),

    TUD_CDC_DESCRIPTOR(USB_ITF_CDC_CONTROL,
                       0,
                       USB_EP_CDC_NOTIFICATION,
                       USB_CDC_NOTIFICATION_SIZE,
                       USB_EP_CDC_OUT,
                       USB_EP_CDC_IN,
                       USB_CDC_DATA_SIZE_HS),

    TUD_HID_DESCRIPTOR(USB_ITF_HID,
                       USB_STR_HID,
                       USB_HID_INTERFACE_PROTOCOL,
                       sizeof(hid_report_descriptor),
                       USB_EP_HID_IN,
                       CFG_TUD_HID_EP_BUFSIZE,
                       USB_HID_POLL_INTERVAL_HS),

    TUD_HID_INOUT_DESCRIPTOR(USB_ITF_VENDOR_HID,
                              USB_STR_VENDOR_HID,
                              false,
                              sizeof(vendor_hid_report_descriptor),
                              USB_EP_VENDOR_HID_OUT,
                              USB_EP_VENDOR_HID_IN,
                              USB_PROTOCOL_REPORT_SIZE,
                              USB_HID_POLL_INTERVAL_HS),

    TUD_AUDIO_DESCRIPTOR(USB_ITF_AUDIO_CONTROL,
                         USB_STR_AUDIO_CONTROL,
                         0,
                         USB_EP_AUDIO_MIC_IN,
                         0),
};

// 编译期锁定接口数量、描述符长度和 IN 端点唯一性。
_Static_assert(USB_ITF_AUDIO_STREAMING_MIC ==
                   USB_ITF_AUDIO_CONTROL + 1,
               "UAC streaming interface must follow its control interface");
_Static_assert(USB_STR_AUDIO_MIC == USB_STR_AUDIO_CONTROL + 1,
               "UAC microphone string must follow the audio control string");
_Static_assert(USB_STR_AUDIO_MIC <
                   sizeof(usb_string_descriptors) /
                       sizeof(usb_string_descriptors[0]),
               "UAC microphone string index must exist in the string table");
_Static_assert(USB_ITF_TOTAL == 6,
               "Composite descriptor must expose six interfaces");
_Static_assert(CFG_TUD_HID == 2,
               "Composite descriptor requires keyboard and vendor HID instances");
_Static_assert(USB_HID_REPORT_ID_KEYBOARD != USB_HID_REPORT_ID_MOUSE,
               "Keyboard and mouse report IDs must be unique");
_Static_assert(USB_HID_INTERFACE_PROTOCOL == HID_ITF_PROTOCOL_NONE,
               "Composite HID interface must not advertise a boot protocol");
_Static_assert(sizeof(hid_keyboard_report_t) == 8U,
               "Unexpected TinyUSB keyboard report size");
_Static_assert(sizeof(hid_mouse_report_t) == 5U,
               "Unexpected TinyUSB mouse report size");
_Static_assert(CFG_TUD_HID_EP_BUFSIZE >= sizeof(hid_keyboard_report_t) + 1U,
               "HID endpoint buffer is too small for report ID and keyboard payload");
_Static_assert(CFG_TUD_HID_EP_BUFSIZE == USB_PROTOCOL_REPORT_SIZE,
               "Vendor HID report size must remain 64 bytes");
_Static_assert(sizeof(bos_descriptor) == USB_BOS_TOTAL_LENGTH,
               "BOS descriptor length mismatch");
_Static_assert(sizeof(usb_full_speed_configuration_descriptor) ==
                   USB_CONFIGURATION_TOTAL_LENGTH,
               "Full-Speed composite descriptor length mismatch");
_Static_assert(sizeof(usb_high_speed_configuration_descriptor) ==
                   USB_CONFIGURATION_TOTAL_LENGTH,
               "High-Speed composite descriptor length mismatch");
_Static_assert(USB_EP_CDC_NOTIFICATION != USB_EP_CDC_IN &&
                   USB_EP_CDC_NOTIFICATION != USB_EP_HID_IN &&
                   USB_EP_CDC_NOTIFICATION != USB_EP_AUDIO_MIC_IN &&
                   USB_EP_CDC_IN != USB_EP_HID_IN &&
                   USB_EP_CDC_IN != USB_EP_AUDIO_MIC_IN &&
                   USB_EP_HID_IN != USB_EP_AUDIO_MIC_IN,
               "IN endpoint addresses must not conflict");
_Static_assert(USB_EP_VENDOR_HID_OUT != USB_EP_CDC_OUT &&
                   USB_EP_VENDOR_HID_OUT != USB_EP_HID_IN &&
                   USB_EP_VENDOR_HID_OUT != USB_EP_AUDIO_MIC_IN &&
                   USB_EP_VENDOR_HID_IN != USB_EP_CDC_NOTIFICATION &&
                   USB_EP_VENDOR_HID_IN != USB_EP_CDC_IN &&
                   USB_EP_VENDOR_HID_IN != USB_EP_HID_IN &&
                   USB_EP_VENDOR_HID_IN != USB_EP_AUDIO_MIC_IN,
               "Vendor HID endpoint addresses must not conflict");

// TinyUSB 回调：按 HID instance 返回键鼠或 Vendor HID Report 描述符。
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == USB_HID_INSTANCE_VENDOR ?
               vendor_hid_report_descriptor : hid_report_descriptor;
}

// TinyUSB 回调：返回用于主机枚举指纹的最小 BOS 描述符。
uint8_t const *tud_descriptor_bos_cb(void)
{
    return bos_descriptor;
}
