/*
 * 文件：usb.h
 * 描述：D50 USB 复合设备公共接口，提供初始化、UAC 取数和键盘鼠标 HID 能力。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// 单一 HID 接口使用的标准键盘修饰键位图。
#define USB_KEY_MODIFIER_LEFT_CTRL             (1U << 0U)
#define USB_KEY_MODIFIER_LEFT_SHIFT            (1U << 1U)
#define USB_KEY_MODIFIER_LEFT_GUI               (1U << 3U)
#define USB_KEY_MODIFIER_RIGHT_ALT             (1U << 6U)
#define USB_KEYBOARD_KEY_COUNT                  (6U)

// 离线固定配置使用的标准 Keyboard/Keypad Page Usage。
enum usb_keycode
{
    USB_KEYCODE_C = 0x06,
    USB_KEYCODE_D = 0x07,
    USB_KEYCODE_V = 0x19,
    USB_KEYCODE_ENTER = 0x28,
    USB_KEYCODE_ESCAPE = 0x29,
    USB_KEYCODE_BACKSPACE = 0x2A,
    USB_KEYCODE_TAB = 0x2B,
    USB_KEYCODE_F1 = 0x3A,
};

// 枚举阶段识别到的主机类型；未知主机由应用层回退到 Windows 配置。
enum usb_host_os
{
    USB_HOST_OS_UNKNOWN,
    USB_HOST_OS_WINDOWS,
    USB_HOST_OS_MACOS,
};

/**
 * @brief USB 麦克风数据读取回调。
 * @param buffer USB 音频数据接收缓冲区。
 * @param size 请求读取的字节数。
 * @param read_size 实际提供的字节数。
 * @param context 调用方上下文。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
typedef esp_err_t (*usb_audio_read_callback_t)(uint8_t *buffer,
                                                   size_t size,
                                                   size_t *read_size,
                                                   void *context);

// USB 初始化配置，回调由 UAC 数据请求上下文调用。
typedef void (*usb_protocol_heartbeat_callback_t)(void *context);

// USB 初始化配置，回调由协议任务在完整校验通过的 H2D 帧后调用。
struct usb_config
{
    usb_audio_read_callback_t audio_read_callback;
    void *audio_context;
    usb_protocol_time_config_callback_t time_config_callback; // 时间同步回调。
    void *time_config_context;                                  // 时间回调上下文。
    usb_protocol_agent_index_callback_t agent_index_callback;  // Agent 图标回调。
    void *agent_index_context;                                  // 图标回调上下文。
    usb_protocol_json_callback_t json_callback;                 // JSON 业务回调。
    void *json_context;                                          // JSON 业务回调。
    usb_protocol_get_device_info_callback_t get_device_info_callback; // 设备信息查询回调。
    void *get_device_info_context;                               // 设备信息查询上下文。
    usb_protocol_factory_serial_callback_t factory_serial_callback; // 产测 SN 写入回调。
    void *factory_serial_context;                                // 产测 SN 回调上下文。
    usb_protocol_key_config_callback_t key_config_callback;       // 按键配置回调。
    void *key_config_context;                                     // 按键配置上下文。
    usb_protocol_heartbeat_callback_t heartbeat_callback;        // H2D 有效帧心跳回调。
    void *heartbeat_context;                                     // 心跳回调上下文。
};

// 标准 6-Key Rollover 键盘输入状态。
struct usb_keyboard_state
{
    uint8_t modifier;
    uint8_t keycodes[USB_KEYBOARD_KEY_COUNT];
};

// 一致读取的 HID 会话状态，键盘提交必须携带其中的 generation。
struct usb_hid_session
{
    uint32_t generation;
    enum usb_host_os host_os;
    bool ready;
};

/**
 * @brief 初始化 D50 USB 2.0 高速 CDC、键盘鼠标 HID 和 UAC2 麦克风复合设备。
 * @param config USB 音频数据回调配置。
 * @return 成功返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
esp_err_t usb_init(const struct usb_config *config);

/**
 * @brief 提交当前完整键盘状态，不会自动追加释放报告。
 * @param expected_generation usb_get_hid_session() 返回的会话代际。
 * @param state 当前修饰键与最多六个普通键的完整状态。
 * @return 当前代际接受状态返回 ESP_OK，参数或会话失效时返回错误。
 * @note FIFO 已满时仍保存当前代际最新状态，接口返回 ESP_OK 并由工作任务收敛。
 */
esp_err_t usb_send_keyboard(uint32_t expected_generation,
                            const struct usb_keyboard_state *state);

/**
 * @brief 将一次相对鼠标滚轮动作加入发送队列。
 * @param wheel 垂直滚轮增量，正数向上、负数向下。
 * @param pan 水平滚轮增量，正数向右、负数向左。
 * @return 成功返回 ESP_OK，参数无效、设备不可用或队列已满时返回错误。
 */
esp_err_t usb_send_mouse_scroll(int8_t wheel, int8_t pan);

/**
 * @brief 取消旧 HID 动作并等待工作任务接收中性状态请求。
 * @return 正常完成返回 ESP_OK，USB 未初始化返回状态错误，50 ms 内未确认返回超时。
 * @note 只能从任务上下文调用；不可发送时会延后到下次挂载后补发中性报告。
 */
esp_err_t usb_hid_cancel(void);

/**
 * @brief 一致读取当前 HID 会话代际、锁定的主机类型和可提交状态。
 * @param session 接收会话状态的结构体。
 * @return 成功返回 ESP_OK，参数无效或 USB 未初始化时返回错误。
 */
esp_err_t usb_get_hid_session(struct usb_hid_session *session);

/**
 * @brief 将一条逻辑消息加入 Vendor HID IN 发送队列。
 * @param message 待发送消息，载荷长度不得超过 USB_PROTOCOL_MAX_PAYLOAD。
 * @return 入队成功返回 ESP_OK，USB 未就绪或队列空间不足返回错误码。
 */
esp_err_t usb_send_protocol_message(const struct usb_protocol_message *message);

#ifdef __cplusplus
}
#endif
