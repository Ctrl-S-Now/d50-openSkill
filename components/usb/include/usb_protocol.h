/*
 * 文件：usb_protocol.h
 * 描述：D50 Vendor HID 逻辑帧编解码和业务命令公共类型。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Vendor HID 单个 Report 的固定字节数，包含未使用区域的 0x00 填充。
#define USB_PROTOCOL_REPORT_SIZE             (64U)
// 逻辑帧同步头由两个连续的 0x7C 字节组成。
#define USB_PROTOCOL_SYNC_BYTE               (0x7CU)
// 逻辑帧头长度：同步头、命令码、协议号和数据长度。
#define USB_PROTOCOL_HEADER_SIZE             (8U)
// 逻辑帧尾的 Checksum16 长度。
#define USB_PROTOCOL_CHECKSUM_SIZE           (2U)
// 逻辑帧除 Payload 外的固定开销。
#define USB_PROTOCOL_FRAME_OVERHEAD          \
    (USB_PROTOCOL_HEADER_SIZE + USB_PROTOCOL_CHECKSUM_SIZE)
// 单条业务命令允许的最大 Payload 长度。
#define USB_PROTOCOL_MAX_PAYLOAD             \
    (1U + 6U * (3U + 65535U))
// 接收解析器和编码器使用的最大完整帧长度。
#define USB_PROTOCOL_MAX_FRAME_SIZE          \
    (USB_PROTOCOL_FRAME_OVERHEAD + USB_PROTOCOL_MAX_PAYLOAD)
// TIME_CONFIG 命令固定使用 7 字节时间字段。
#define USB_PROTOCOL_TIME_CONFIG_SIZE        (7U)
// GET_DEVICE_INFO 和产测 SN JSON 的协议长度上限。
#define USB_PROTOCOL_DEVICE_INFO_MAX_SIZE    (256U)
// SET_KEY_CONFIG 的 JSON 载荷上限。
#define USB_PROTOCOL_KEY_CONFIG_MAX_SIZE     (1024U)
// CONTROL_EVENT 固定为 ControlType、Index/JoystickX、Action/JoystickY。
#define USB_PROTOCOL_CONTROL_EVENT_SIZE     (3U)

// 逻辑帧 CommandCode，描述主机与设备之间的传输方向和响应类型。
enum usb_protocol_command_code
{
    USB_PROTOCOL_COMMAND_H2D = 0x00,       // 主机下发命令到设备。
    USB_PROTOCOL_COMMAND_D2H = 0x01,       // 设备主动上报事件到主机。
    USB_PROTOCOL_COMMAND_H_RESPONSE = 0x02, // 主机响应，当前版本保留。
    USB_PROTOCOL_COMMAND_D_RESPONSE = 0x03, // 设备返回查询结果。
};

// 当前已实现的业务 ProtocolID。
enum usb_protocol_id
{
    USB_PROTOCOL_ID_GET_DEVICE_INFO = 0x01, // 查询设备信息。
    USB_PROTOCOL_ID_SET_KEY_CONFIG = 0x02, // 设置五个普通按键配置。
    USB_PROTOCOL_ID_CONTROL_EVENT = 0x03, // 上报按键或摇杆事件。
    USB_PROTOCOL_ID_TIME_CONFIG = 0x04, // 设置设备当前时间。
    USB_PROTOCOL_ID_AGENT_INDEX = 0x05, // 设置 Agent PNG 图标。
    USB_PROTOCOL_ID_CONVERSATION_LIST = 0x06, // 全量设置 Agent 会话。
    USB_PROTOCOL_ID_UPSERT_CONVERSATION = 0x07, // 新增或更新一个会话。
    USB_PROTOCOL_ID_CONVERSATION_ACTION = 0x08, // 上报会话按钮操作。
    USB_PROTOCOL_ID_AGENT_BALANCE = 0x09, // 全量设置 Agent 余额。
    // 0x0A 已被上位机预留给未来“旋钮版”D2H INPUT_EVENT，日程改用 0x0B 避免歧义。
    USB_PROTOCOL_ID_SCHEDULE = 0x0B, // 全量设置当日日程列表（H2D）。
    USB_PROTOCOL_ID_FACTORY_SERIAL = 0xF1, // 产测写入设备序列号。
};

// CONTROL_EVENT 的三种控制对象。
enum usb_protocol_control_type
{
    USB_PROTOCOL_CONTROL_POP = 0U,
    USB_PROTOCOL_CONTROL_KEY = 1U,
    USB_PROTOCOL_CONTROL_JOYSTICK = 2U,
};

// KEY/POP 的动作只上报完成后的单击或长按生命周期事件。
enum usb_protocol_control_action
{
    USB_PROTOCOL_ACTION_CLICK = 0U,
    USB_PROTOCOL_ACTION_LONG_PRESSED = 1U,
    USB_PROTOCOL_ACTION_LONG_RELEASED = 2U,
};

// 已完成校验的逻辑消息，Payload 长度由 data_length 指定。
struct usb_protocol_message
{
    uint8_t command_code;  // CommandCode，取值见 usb_protocol_command_code。
    uint8_t protocol_id;   // ProtocolID，取值见 usb_protocol_id。
    uint32_t data_length;  // Payload 的实际字节数，不包含帧头和校验。
    const uint8_t *payload; // 指向解析器缓冲区，命令回调完成前有效。
};

// TIME_CONFIG 的 7 字节载荷，字段顺序与协议文档保持一致。
struct usb_protocol_time_config
{
    uint16_t year;   // 公历年份，使用小端序编码。
    uint8_t month;   // 月份，范围 1～12。
    uint8_t day;     // 日期，按月份和闰年规则校验。
    uint8_t hour;    // 小时，范围 0～23。
    uint8_t minute;  // 分钟，范围 0～59。
    uint8_t second;  // 秒，范围 0～59。
};

// app 通过该回调接收已校验的时间值；回调不得保存 time_config 指针。
typedef esp_err_t (*usb_protocol_time_config_callback_t)(
    const struct usb_protocol_time_config *time_config,
    void *context);

// app 通过该回调接收已校验的 Agent 图标 Payload；回调不得保存 payload 指针。
typedef esp_err_t (*usb_protocol_agent_index_callback_t)(
    const uint8_t *payload,
    size_t payload_length,
    void *context);

// app 通过该回调接收已收齐的 JSON 业务载荷；回调不得保存 payload 指针。
typedef esp_err_t (*usb_protocol_json_callback_t)(
    uint8_t protocol_id,
    const uint8_t *payload,
    size_t payload_length,
    void *context);

// app 通过该回调响应无 Payload 的设备信息查询。
typedef esp_err_t (*usb_protocol_get_device_info_callback_t)(void *context);

// app 通过该回调接收产测 SN JSON；回调不得保存 payload 指针。
typedef esp_err_t (*usb_protocol_factory_serial_callback_t)(
    const uint8_t *payload,
    size_t payload_length,
    void *context);

// app 通过该回调接收完整的按键配置 JSON；回调不得保存 payload 指针。
typedef esp_err_t (*usb_protocol_key_config_callback_t)(
    const uint8_t *payload,
    size_t payload_length,
    void *context);

// 接收解析器状态，USB 断连、挂起或队列丢包时必须复位。
struct usb_protocol_parser
{
    uint8_t *frame;             // 当前逻辑帧的动态缓存，优先分配在 PSRAM。
    size_t frame_capacity;      // frame 的容量。
    size_t frame_length;        // 已接收的逻辑帧字节数。
    size_t expected_length;     // 根据 DataLength 计算出的帧总长。
    bool receiving;             // 是否正在等待后续 Report。
};

/**
 * @brief 清空 Vendor HID 接收解析器状态。
 * @param parser 解析器实例。
 */
void usb_protocol_parser_reset(struct usb_protocol_parser *parser);

/**
 * @brief 解析一个完整的 64 字节 Vendor HID 报告。
 * @param parser 解析器实例。
 * @param report 固定 64 字节报告。
 * @param message 接收完成消息的输出结构体；payload 指针在 parser_reset 前有效。
 * @param complete 输出 true 表示本次报告完成了一条逻辑帧。
 * @return 参数正确且报告尚未完成时返回 ESP_OK；协议错误返回具体错误码。
 */
esp_err_t usb_protocol_feed_report(struct usb_protocol_parser *parser,
                                   const uint8_t report[USB_PROTOCOL_REPORT_SIZE],
                                   struct usb_protocol_message *message,
                                   bool *complete);

/**
 * @brief 将逻辑消息编码为连续逻辑帧。
 * @param message 待编码消息。
 * @param frame 输出帧缓冲区。
 * @param frame_capacity frame 的容量。
 * @param frame_length 输出实际帧长度。
 * @return 编码成功返回 ESP_OK，参数或容量不足返回错误码。
 */
esp_err_t usb_protocol_encode_frame(const struct usb_protocol_message *message,
                                    uint8_t *frame,
                                    size_t frame_capacity,
                                    size_t *frame_length);

#ifdef __cplusplus
}
#endif
