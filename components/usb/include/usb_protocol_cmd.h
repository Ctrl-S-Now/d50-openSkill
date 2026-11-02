/*
 * 文件：usb_protocol_cmd.h
 * 描述：D50 Vendor HID 业务命令校验和分发接口。
 */

#pragma once

#include "usb_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按 CommandCode 和 ProtocolID 分发并执行一条已完成的逻辑消息。
 * @param message 已通过帧校验的消息。
 * @param time_callback TIME_CONFIG 的应用层回调；执行时间同步时不能为空。
 * @param time_context 传递给 time_callback 的应用上下文指针。
 * @param agent_index_callback SET_AGENT_INDEX 的应用层回调；执行图标配置时不能为空。
 * @param agent_index_context 传递给 agent_index_callback 的应用上下文指针。
 * @param json_callback 会话和余额 JSON 的应用层回调。
 * @param json_context 传递给 json_callback 的应用上下文指针。
 * @param get_device_info_callback GET_DEVICE_INFO 查询回调。
 * @param get_device_info_context 设备信息查询回调上下文。
 * @param factory_serial_callback 产测 SN JSON 写入回调。
 * @param factory_serial_context 产测 SN 回调上下文。
 * @param key_config_callback SET_KEY_CONFIG JSON 回调。
 * @param key_config_context 按键配置回调上下文。
 * @return 命令执行结果；不支持的命令返回 ESP_ERR_NOT_SUPPORTED。
 */
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
    void *key_config_context);

#ifdef __cplusplus
}
#endif
