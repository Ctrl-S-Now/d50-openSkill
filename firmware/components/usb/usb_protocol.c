/*
 * 文件：usb_protocol.c
 * 描述：D50 Vendor HID 64 字节逻辑帧编解码。
 */

#include "usb_protocol.h"

#include <string.h>

#include "esp_heap_caps.h"

// 按小端序读取 32 位无符号整数。
static uint32_t read_u32_le(const uint8_t *data)
{
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

// 按小端序写入 32 位无符号整数。
static void write_u32_le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8U) & 0xFFU);
    data[2] = (uint8_t)((value >> 16U) & 0xFFU);
    data[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

// 对指定范围的协议字节进行 16 位累加校验。
static uint16_t checksum16(const uint8_t *data, size_t length)
{
    uint32_t sum = 0U; // 使用 32 位累加，最后截断为协议要求的 16 位。

    for (size_t index = 0U; index < length; index++)
    {
        sum += data[index];
    }
    return (uint16_t)sum;
}

// 返回两个长度值中的较小值，用于限制单次拷贝大小。
static size_t minimum_size(size_t left, size_t right)
{
    return left < right ? left : right;
}

// 释放动态解析缓存并清空接收状态。
void usb_protocol_parser_reset(struct usb_protocol_parser *parser)
{
    if (parser == NULL)
    {
        return;
    }
    if (parser->frame != NULL)
    {
        heap_caps_free(parser->frame);
    }
    parser->frame = NULL;
    parser->frame_capacity = 0U;
    parser->frame_length = 0U;
    parser->expected_length = 0U;
    parser->receiving = false;
}

// 接收一个 64 字节 Report，并在逻辑帧完整且校验通过时输出消息。
esp_err_t usb_protocol_feed_report(struct usb_protocol_parser *parser,
                                   const uint8_t report[USB_PROTOCOL_REPORT_SIZE],
                                   struct usb_protocol_message *message,
                                   bool *complete)
{
    if (parser == NULL || report == NULL || message == NULL || complete == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *complete = false;
    if (!parser->receiving)
    {
        // 上一条完整消息的 Payload 已由调用方消费；开始新帧前释放旧缓存。
        usb_protocol_parser_reset(parser);
        if (report[0] != USB_PROTOCOL_SYNC_BYTE ||
            report[1] != USB_PROTOCOL_SYNC_BYTE)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }

        // 首个 Report 携带帧头，据此计算后续需要接收的总长度。
        uint32_t payload_length = read_u32_le(&report[4]);
        if (payload_length > USB_PROTOCOL_MAX_PAYLOAD)
        {
            usb_protocol_parser_reset(parser);
            return ESP_ERR_INVALID_SIZE;
        }

        parser->expected_length = USB_PROTOCOL_FRAME_OVERHEAD + payload_length;
        parser->frame = heap_caps_malloc(parser->expected_length,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (parser->frame == NULL)
        {
            parser->frame = heap_caps_malloc(parser->expected_length,
                                             MALLOC_CAP_8BIT);
        }
        if (parser->frame == NULL)
        {
            usb_protocol_parser_reset(parser);
            return ESP_ERR_NO_MEM;
        }
        parser->frame_capacity = parser->expected_length;
        parser->frame_length = minimum_size(USB_PROTOCOL_REPORT_SIZE,
                                            parser->expected_length);
        memcpy(parser->frame, report, parser->frame_length);
        parser->receiving = parser->frame_length < parser->expected_length;
    }
    else
    {
        if (parser->frame == NULL ||
            parser->frame_capacity < parser->expected_length ||
            parser->frame_length >= parser->expected_length)
        {
            usb_protocol_parser_reset(parser);
            return ESP_ERR_INVALID_STATE;
        }
        size_t remaining = parser->expected_length - parser->frame_length;
        size_t copy_length = minimum_size(USB_PROTOCOL_REPORT_SIZE, remaining);
        memcpy(parser->frame + parser->frame_length, report, copy_length);
        parser->frame_length += copy_length;
    }

    if (parser->frame_length < parser->expected_length)
    {
        return ESP_OK;
    }

    // 帧已收齐，先校验帧尾，再把 Payload 交给业务层。
    size_t payload_length = parser->expected_length - USB_PROTOCOL_FRAME_OVERHEAD;
    uint16_t expected_checksum = (uint16_t)parser->frame[8U + payload_length] |
                                  ((uint16_t)parser->frame[9U + payload_length] << 8U);
    uint16_t actual_checksum = checksum16(&parser->frame[2],
                                          6U + payload_length);
    if (expected_checksum != actual_checksum)
    {
        usb_protocol_parser_reset(parser);
        return ESP_ERR_INVALID_CRC;
    }

    message->command_code = parser->frame[2];
    message->protocol_id = parser->frame[3];
    message->data_length = (uint32_t)payload_length;
    message->payload = payload_length > 0U ? &parser->frame[8] : NULL;
    // 保留 frame，供命令回调读取；协议任务在命令执行后调用 reset 释放它。
    parser->receiving = false;
    *complete = true;
    return ESP_OK;
}

// 将业务消息编码为连续逻辑帧，调用方负责按 64 字节 Report 分包发送。
esp_err_t usb_protocol_encode_frame(const struct usb_protocol_message *message,
                                    uint8_t *frame,
                                    size_t frame_capacity,
                                    size_t *frame_length)
{
    if (message == NULL || frame == NULL || frame_length == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (message->data_length > USB_PROTOCOL_MAX_PAYLOAD)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    if (message->data_length > 0U && message->payload == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = USB_PROTOCOL_FRAME_OVERHEAD + message->data_length;
    if (frame_capacity < length)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    frame[0] = USB_PROTOCOL_SYNC_BYTE;
    frame[1] = USB_PROTOCOL_SYNC_BYTE;
    frame[2] = message->command_code;
    frame[3] = message->protocol_id;
    write_u32_le(&frame[4], message->data_length);
    if (message->data_length > 0U)
    {
        memcpy(&frame[8], message->payload, message->data_length);
    }

    uint16_t sum = checksum16(&frame[2], 6U + message->data_length);
    frame[8U + message->data_length] = (uint8_t)(sum & 0xFFU);
    frame[9U + message->data_length] = (uint8_t)(sum >> 8U);
    *frame_length = length;
    return ESP_OK;
}
