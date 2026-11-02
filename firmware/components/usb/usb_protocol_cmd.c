/*
 * 文件：usb_protocol_cmd.c
 * 描述：D50 Vendor HID 业务命令校验和分发执行器。
 */

#include "usb_protocol_cmd.h"

// 判断公历年份是否为闰年。
static bool is_leap_year(uint16_t year)
{
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

// 返回指定月份的天数；调用方需保证 month 位于 1～12。
static uint8_t month_days(uint16_t year, uint8_t month)
{
    // 平年各月份天数，二月在闰年时单独修正。
    static const uint8_t days[] = {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U,
    };

    if (month == 2U && is_leap_year(year))
    {
        return 29U;
    }
    return days[month - 1U];
}

// 校验 TIME_CONFIG 的日期和时间字段是否在协议允许范围内。
static bool time_config_is_valid(const struct usb_protocol_time_config *time_config)
{
    if (time_config->year < 2000U || time_config->year > 2099U ||
        time_config->month < 1U || time_config->month > 12U ||
        time_config->hour > 23U || time_config->minute > 59U ||
        time_config->second > 59U)
    {
        return false;
    }
    return time_config->day >= 1U &&
           time_config->day <= month_days(time_config->year,
                                          time_config->month);
}

// 分发当前已支持的业务命令，并调用应用层回调。
esp_err_t usb_protocol_cmd_execute(
    const struct usb_protocol_message *message,
    usb_protocol_time_config_callback_t time_callback,
    void *time_context,
    usb_protocol_agent_index_callback_t agent_index_callback,
    void *agent_index_context,
    usb_protocol_json_callback_t json_callback,
    void *json_context,
    usb_protocol_get_device_info_callback_t get_device_info_callback,
    void *get_device_info_context,
    usb_protocol_factory_serial_callback_t factory_serial_callback,
    void *factory_serial_context,
    usb_protocol_key_config_callback_t key_config_callback,
    void *key_config_context)
{
    if (message == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (message->command_code != USB_PROTOCOL_COMMAND_H2D)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (message->protocol_id == USB_PROTOCOL_ID_GET_DEVICE_INFO)
    {
        if (message->data_length != 0U || message->payload != NULL)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        if (get_device_info_callback == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
        return get_device_info_callback(get_device_info_context);
    }

    if (message->protocol_id == USB_PROTOCOL_ID_FACTORY_SERIAL)
    {
        if (message->data_length == 0U ||
            message->data_length > USB_PROTOCOL_DEVICE_INFO_MAX_SIZE ||
            message->payload == NULL)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        if (factory_serial_callback == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
        return factory_serial_callback(message->payload,
                                       (size_t)message->data_length,
                                       factory_serial_context);
    }

    if (message->protocol_id == USB_PROTOCOL_ID_SET_KEY_CONFIG)
    {
        if (message->data_length == 0U ||
            message->data_length > USB_PROTOCOL_KEY_CONFIG_MAX_SIZE ||
            message->payload == NULL)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        if (key_config_callback == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
        return key_config_callback(message->payload,
                                   (size_t)message->data_length,
                                   key_config_context);
    }

    if (message->protocol_id == USB_PROTOCOL_ID_AGENT_INDEX)
    {
        if (message->data_length < 1U)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        if (agent_index_callback == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
        return agent_index_callback(message->payload,
                                    (size_t)message->data_length,
                                    agent_index_context);
    }

    if (message->protocol_id == USB_PROTOCOL_ID_CONVERSATION_LIST ||
        message->protocol_id == USB_PROTOCOL_ID_UPSERT_CONVERSATION ||
        message->protocol_id == USB_PROTOCOL_ID_AGENT_BALANCE ||
        message->protocol_id == USB_PROTOCOL_ID_SCHEDULE)
    {
        if (message->data_length == 0U || message->payload == NULL)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        if (json_callback == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
        return json_callback(message->protocol_id,
                             message->payload,
                             (size_t)message->data_length,
                             json_context);
    }

    if (message->protocol_id != USB_PROTOCOL_ID_TIME_CONFIG)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (message->data_length != USB_PROTOCOL_TIME_CONFIG_SIZE ||
        message->payload == NULL)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    if (time_callback == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // 协议中的年份为小端序，其余字段按字节顺序排列。
    struct usb_protocol_time_config time_config = {
        .year = (uint16_t)message->payload[0] |
                ((uint16_t)message->payload[1] << 8U),
        .month = message->payload[2],
        .day = message->payload[3],
        .hour = message->payload[4],
        .minute = message->payload[5],
        .second = message->payload[6],
    };
    if (!time_config_is_valid(&time_config))
    {
        return ESP_ERR_INVALID_ARG;
    }
    return time_callback(&time_config, time_context);
}
