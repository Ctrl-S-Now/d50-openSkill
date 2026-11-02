/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 文件：usb_device.c
 * 描述：运行 D50 USB 2.0 高速 CDC、键盘鼠标 HID、Vendor HID 和 UAC2 麦克风复合设备。
 * 约束：USB 回调只维护状态或投递数据，耗时发送由工作任务完成。
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "device/dcd.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "usb_device_uac.h"

#include "usb.h"
#include "device_descriptors.h"
#include "usb_protocol_cmd.h"

#if CONFIG_USB_CDC_AUTO_DOWNLOAD
#include "soc/soc.h"
#include "soc/lp_system_reg.h"

// P4 ROM 里存在但 IDF 未对外封装的 USB persist / 复位入口，直接按符号引用。
// USBDC_PERSIST_ENA 取自 esp32s3/rom/usb/usb_persist.h，值为 (1<<31)。
#define USB_CDC_PERSIST_ENA (1U << 31)
extern void usb_dc_prepare_persist(void);
extern void chip_usb_set_persist_flags(uint32_t flags);
extern void software_reset_cpu(int cpu_no);
#endif // CONFIG_USB_CDC_AUTO_DOWNLOAD

// HID 动作队列、任务资源、取消等待和服务节拍配置。
#define USB_HID_QUEUE_LENGTH                    (16U)
#define USB_HID_TASK_STACK_SIZE                 (3072U)
#define USB_HID_TASK_PRIORITY                   (5U)
#define USB_HID_SERVICE_PERIOD_MS               (1U)
#define USB_HID_QUEUE_WAIT_MS                    (10U)
#define USB_HID_CANCEL_TIMEOUT_MS                (50U)
#define USB_HID_DROP_LOG_INTERVAL_MS            (1000U)

// Vendor HID 协议队列和任务资源配置。
#define USB_PROTOCOL_RX_QUEUE_LENGTH             (64U)
#define USB_PROTOCOL_TX_QUEUE_LENGTH             (16U)
#define USB_PROTOCOL_TASK_STACK_SIZE             (4096U)
#define USB_PROTOCOL_TASK_PRIORITY               (5U)
#define USB_PROTOCOL_SERVICE_PERIOD_MS           (1U)
#define USB_PROTOCOL_FRAME_TIMEOUT_MS            (250U)
#define USB_PROTOCOL_DROP_LOG_INTERVAL_MS        (1000U)

// CDC 日志镜像队列、单条日志上限和任务资源配置。
#define USB_CDC_LOG_QUEUE_LENGTH                (24U)
#define USB_CDC_LOG_CHUNK_SIZE                  (192U)
#define USB_CDC_TASK_STACK_SIZE                 (4096U)
#define USB_CDC_TASK_PRIORITY                   (4U)

typedef struct
{
    // 日志长度不包含字符串终止符，发送时以该字段为准。
    uint16_t length;
    char data[USB_CDC_LOG_CHUNK_SIZE];
} usb_cdc_log_item_t;

// HID 队列项只保存完整键盘状态或滚轮动作及其创建代际。
enum usb_hid_action_type
{
    USB_HID_ACTION_KEYBOARD,
    USB_HID_ACTION_MOUSE_SCROLL,
};

struct usb_hid_action
{
    uint32_t generation;
    enum usb_hid_action_type type;
    union
    {
        struct
        {
            uint32_t revision;
            struct usb_keyboard_state state;
        } keyboard;
        struct
        {
            int8_t wheel;
            int8_t pan;
        } mouse;
    } data;
};

// Vendor HID 报告固定为 64 字节，队列项不携带动态内存或指针。
struct usb_protocol_report
{
    uint8_t data[USB_PROTOCOL_REPORT_SIZE];
};

// 新代际必须依次发送键盘全释放和鼠标全零报告。
enum usb_hid_neutral_stage
{
    USB_HID_NEUTRAL_KEYBOARD,
    USB_HID_NEUTRAL_MOUSE,
    USB_HID_NEUTRAL_COMPLETE,
};

static const char *tag = "usb";
// 初始化时复制应用配置，供 UAC 回调长期使用。
static struct usb_config usb_config;
// HID 和 CDC 使用独立队列与任务，避免在调用方或日志钩子中阻塞 USB 栈。
static QueueHandle_t hid_queue;
static QueueHandle_t protocol_rx_queue;
static QueueHandle_t protocol_tx_queue;
static QueueHandle_t cdc_log_queue;
static TaskHandle_t hid_task_handle;
static TaskHandle_t protocol_task_handle;
static TaskHandle_t cdc_task_handle;
// 原始日志输出函数始终保留，CDC 仅作为尽力而为的镜像通道。
static vprintf_like_t uart_vprintf = vprintf;
// 初始化与总线状态由不同上下文访问，总线状态通过原子操作同步。
static bool init_started;
static bool log_mirror_enabled;
static uint32_t initialized;
static uint32_t device_mounted;
static uint32_t device_suspended;
static uint32_t hid_generation = 1U;
static uint32_t hid_ack_generation;
static uint32_t hid_dropped_count;
static uint32_t protocol_dropped_count;
static uint32_t protocol_reset_requested;
// 键盘 FIFO 溢出时由当前代际的最新完整状态保证最终收敛。
static portMUX_TYPE keyboard_state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t keyboard_state_revision;
static uint32_t desired_keyboard_generation;
static uint32_t desired_keyboard_revision;
static struct usb_keyboard_state desired_keyboard_state;
// OS 枚举指纹由 DCD 包装函数写入，并在挂载事件中锁定。
static uint32_t os_enumeration_generation;
static uint32_t os_descriptor_first;
static uint32_t os_descriptor_second;
static uint32_t host_os_locked;
static uint32_t current_host_os = USB_HOST_OS_UNKNOWN;

_Static_assert(sizeof(struct usb_protocol_report) == USB_PROTOCOL_REPORT_SIZE,
               "Vendor HID report queue item must be 64 bytes");

_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0),
               "USB ISR state requires lock-free 32-bit atomics");

/*
 * 原始 TinyUSB DCD 事件处理函数由链接器 --wrap 解析。
 * 包装函数可能在 ISR 中运行，只允许固定时间的 lock-free 原子操作。
 */
void __real_dcd_event_handler(const dcd_event_t *event, bool in_isr);

TU_ATTR_FAST_FUNC void __wrap_dcd_event_handler(const dcd_event_t *event, bool in_isr)
{
    if (event->event_id == DCD_EVENT_BUS_RESET)
    {
        __atomic_fetch_add(&os_enumeration_generation, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&os_descriptor_first, 0U, __ATOMIC_RELAXED);
        __atomic_store_n(&os_descriptor_second, 0U, __ATOMIC_RELAXED);
        __atomic_store_n(&host_os_locked, 0U, __ATOMIC_RELAXED);
        __atomic_store_n(&current_host_os, USB_HOST_OS_UNKNOWN, __ATOMIC_RELAXED);
        __atomic_store_n(&device_mounted, 0U, __ATOMIC_RELAXED);
        __atomic_store_n(&device_suspended, 0U, __ATOMIC_RELAXED);
        __atomic_fetch_add(&hid_generation, 1U, __ATOMIC_RELEASE);
        __atomic_store_n(&protocol_reset_requested, 1U, __ATOMIC_RELEASE);
    }
    else if (event->event_id == DCD_EVENT_SETUP_RECEIVED &&
             event->setup_received.bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
             event->setup_received.bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE &&
             event->setup_received.bRequest == TUSB_REQ_GET_DESCRIPTOR &&
             __atomic_load_n(&host_os_locked, __ATOMIC_RELAXED) == 0U)
    {
        uint32_t descriptor = tu_u16_high(event->setup_received.wValue);
        if (descriptor == TUSB_DESC_STRING ||
            descriptor == TUSB_DESC_CONFIGURATION ||
            descriptor == TUSB_DESC_BOS)
        {
            uint32_t first = __atomic_load_n(&os_descriptor_first, __ATOMIC_RELAXED);
            if (first == 0U)
            {
                __atomic_store_n(&os_descriptor_first, descriptor, __ATOMIC_RELAXED);
            }
            else if (descriptor != first &&
                     __atomic_load_n(&os_descriptor_second, __ATOMIC_RELAXED) == 0U)
            {
                __atomic_store_n(&os_descriptor_second, descriptor, __ATOMIC_RELAXED);
            }
        }
    }

    __real_dcd_event_handler(event, in_isr);
}

// 设备已挂载且未挂起时，HID 端点才允许发送报告。
static bool _hid_available(void)
{
    return __atomic_load_n(&device_mounted, __ATOMIC_ACQUIRE) != 0U &&
           __atomic_load_n(&device_suspended, __ATOMIC_ACQUIRE) == 0U;
}

// 根据枚举阶段前两个不同的描述符请求锁定本次主机系统类型。
static enum usb_host_os _lock_host_os(void)
{
    uint32_t first = __atomic_load_n(&os_descriptor_first, __ATOMIC_ACQUIRE);
    uint32_t second = __atomic_load_n(&os_descriptor_second, __ATOMIC_ACQUIRE);
    enum usb_host_os host_os = USB_HOST_OS_UNKNOWN;

    if (first == TUSB_DESC_CONFIGURATION && second == TUSB_DESC_BOS)
    {
        host_os = USB_HOST_OS_WINDOWS;
    }
    else if (first == TUSB_DESC_STRING &&
             (second == TUSB_DESC_CONFIGURATION || second == TUSB_DESC_BOS))
    {
        host_os = USB_HOST_OS_MACOS;
    }

    __atomic_store_n(&current_host_os, (uint32_t)host_os, __ATOMIC_RELEASE);
    __atomic_store_n(&host_os_locked, 1U, __ATOMIC_RELEASE);
    return host_os;
}

// TinyUSB 总线事件回调只更新状态和 HID 代际，清理由 HID 工作任务延后完成。
static void _usb_event_callback(tinyusb_event_t *event, void *argument)
{
    (void)argument;

    switch (event->id)
    {
    case TINYUSB_EVENT_ATTACHED:
    {
        enum usb_host_os host_os;

        if (__atomic_load_n(&host_os_locked, __ATOMIC_ACQUIRE) == 0U)
        {
            host_os = _lock_host_os();
        }
        else
        {
            host_os = (enum usb_host_os)__atomic_load_n(&current_host_os,
                                                         __ATOMIC_ACQUIRE);
        }
        // OS 结果先锁定，最后发布 mounted，保证会话快照不会看到半初始化状态。
        __atomic_store_n(&device_suspended, 0U, __ATOMIC_RELEASE);
        __atomic_fetch_add(&hid_generation, 1U, __ATOMIC_RELEASE);
        __atomic_store_n(&device_mounted, 1U, __ATOMIC_RELEASE);

        uint32_t generation = __atomic_load_n(&os_enumeration_generation,
                                               __ATOMIC_ACQUIRE);
        if (host_os == USB_HOST_OS_MACOS)
        {
            ESP_LOGI(tag, "USB host OS: macOS, enumeration=%lu",
                     (unsigned long)generation);
        }
        else if (host_os == USB_HOST_OS_WINDOWS)
        {
            ESP_LOGI(tag, "USB host OS: Windows, enumeration=%lu",
                     (unsigned long)generation);
        }
        else
        {
            ESP_LOGW(tag, "USB host OS unknown, use Windows profile, enumeration=%lu",
                     (unsigned long)generation);
        }
        break;
    }

    case TINYUSB_EVENT_DETACHED:
        __atomic_store_n(&device_mounted, 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&device_suspended, 0U, __ATOMIC_RELEASE);
        __atomic_fetch_add(&hid_generation, 1U, __ATOMIC_RELEASE);
        __atomic_store_n(&protocol_reset_requested, 1U, __ATOMIC_RELEASE);
        break;

#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    case TINYUSB_EVENT_SUSPENDED:
        __atomic_store_n(&device_suspended, 1U, __ATOMIC_RELEASE);
        __atomic_fetch_add(&hid_generation, 1U, __ATOMIC_RELEASE);
        __atomic_store_n(&protocol_reset_requested, 1U, __ATOMIC_RELEASE);
        break;
#endif

#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
    case TINYUSB_EVENT_RESUMED:
        __atomic_store_n(&device_suspended, 0U, __ATOMIC_RELEASE);
        __atomic_fetch_add(&hid_generation, 1U, __ATOMIC_RELEASE);
        __atomic_store_n(&protocol_reset_requested, 1U, __ATOMIC_RELEASE);
        break;
#endif

    default:
        break;
    }
}

// 32 位修订号采用回绕安全比较，只在差值小于半个计数空间时判定为更新。
static bool _revision_is_newer(uint32_t revision, uint32_t reference)
{
    return (int32_t)(revision - reference) > 0;
}

// 在短临界区内复制当前 HID 代际尚未发送的最新键盘状态。
static bool _get_latest_keyboard_action(uint32_t generation,
                                        uint32_t sent_revision,
                                        bool sent_revision_valid,
                                        struct usb_hid_action *action)
{
    bool available = false;

    portENTER_CRITICAL(&keyboard_state_lock);
    if (desired_keyboard_generation == generation &&
        (!sent_revision_valid ||
         _revision_is_newer(desired_keyboard_revision, sent_revision)))
    {
        action->generation = generation;
        action->type = USB_HID_ACTION_KEYBOARD;
        action->data.keyboard.revision = desired_keyboard_revision;
        action->data.keyboard.state = desired_keyboard_state;
        available = true;
    }
    portEXIT_CRITICAL(&keyboard_state_lock);
    return available;
}

/*
 * HID 工作任务先为每个新代际发送键盘和鼠标中性报告，再串行发送动作。
 * 队列项代际不匹配时直接丢弃，端点未就绪时保留当前发送阶段等待重试。
 */
static void _hid_task(void *argument)
{
    struct usb_hid_action action = {0};
    uint32_t active_generation = 0U;
    uint32_t sent_keyboard_revision = 0U;
    bool sent_keyboard_revision_valid = false;
    bool have_action = false;
    enum usb_hid_neutral_stage neutral_stage = USB_HID_NEUTRAL_KEYBOARD;
    TickType_t last_drop_log = 0U;

    (void)argument;
    for (;;)
    {
        TickType_t now = xTaskGetTickCount();
        if ((now - last_drop_log) >= pdMS_TO_TICKS(USB_HID_DROP_LOG_INTERVAL_MS))
        {
            uint32_t dropped = __atomic_exchange_n(&hid_dropped_count, 0U, __ATOMIC_RELAXED);
            if (dropped > 0U)
            {
                ESP_LOGW(tag, "Dropped %lu HID action(s)", (unsigned long)dropped);
            }
            last_drop_log = now;
        }

        uint32_t generation = __atomic_load_n(&hid_generation, __ATOMIC_ACQUIRE);
        if (generation != active_generation)
        {
            active_generation = generation;
            have_action = false;
            sent_keyboard_revision = 0U;
            sent_keyboard_revision_valid = false;
            neutral_stage = USB_HID_NEUTRAL_KEYBOARD;
        }

        if (!_hid_available())
        {
            // 当前无法发送时先确认本地失效，恢复后仍从双中性报告开始。
            __atomic_store_n(&hid_ack_generation, active_generation, __ATOMIC_RELEASE);
            vTaskDelay(pdMS_TO_TICKS(USB_HID_SERVICE_PERIOD_MS));
            continue;
        }

        if (neutral_stage != USB_HID_NEUTRAL_COMPLETE)
        {
            if (!tud_hid_ready())
            {
                vTaskDelay(pdMS_TO_TICKS(USB_HID_SERVICE_PERIOD_MS));
                continue;
            }
            if (neutral_stage == USB_HID_NEUTRAL_KEYBOARD &&
                tud_hid_keyboard_report(USB_HID_REPORT_ID_KEYBOARD, 0U, NULL))
            {
                neutral_stage = USB_HID_NEUTRAL_MOUSE;
            }
            else if (neutral_stage == USB_HID_NEUTRAL_MOUSE &&
                     tud_hid_mouse_report(USB_HID_REPORT_ID_MOUSE,
                                          0U, 0, 0, 0, 0))
            {
                neutral_stage = USB_HID_NEUTRAL_COMPLETE;
                __atomic_store_n(&hid_ack_generation,
                                 active_generation,
                                 __ATOMIC_RELEASE);
            }
            vTaskDelay(pdMS_TO_TICKS(USB_HID_SERVICE_PERIOD_MS));
            continue;
        }

        if (!have_action)
        {
            if (xQueueReceive(hid_queue,
                              &action,
                              pdMS_TO_TICKS(USB_HID_QUEUE_WAIT_MS)) != pdTRUE)
            {
                if (!_get_latest_keyboard_action(active_generation,
                                                 sent_keyboard_revision,
                                                 sent_keyboard_revision_valid,
                                                 &action))
                {
                    continue;
                }
            }
            if (action.generation != active_generation)
            {
                continue;
            }
            if (action.type == USB_HID_ACTION_KEYBOARD &&
                sent_keyboard_revision_valid &&
                !_revision_is_newer(action.data.keyboard.revision,
                                    sent_keyboard_revision))
            {
                continue;
            }
            have_action = true;
        }

        if (!tud_hid_ready())
        {
            vTaskDelay(pdMS_TO_TICKS(USB_HID_SERVICE_PERIOD_MS));
            continue;
        }

        // 代际可能在端点等待期间变化，旧状态不得进入新的主机会话。
        if (__atomic_load_n(&hid_generation, __ATOMIC_ACQUIRE) != active_generation)
        {
            continue;
        }

        if (action.type == USB_HID_ACTION_KEYBOARD)
        {
            if (tud_hid_keyboard_report(USB_HID_REPORT_ID_KEYBOARD,
                                        action.data.keyboard.state.modifier,
                                        action.data.keyboard.state.keycodes))
            {
                sent_keyboard_revision = action.data.keyboard.revision;
                sent_keyboard_revision_valid = true;
                have_action = false;
            }
        }
        else if (action.type == USB_HID_ACTION_MOUSE_SCROLL &&
                 tud_hid_mouse_report(USB_HID_REPORT_ID_MOUSE,
                                      0U,
                                      0,
                                      0,
                                      action.data.mouse.wheel,
                                      action.data.mouse.pan))
        {
            have_action = false;
        }
        else if (action.type != USB_HID_ACTION_KEYBOARD &&
                 action.type != USB_HID_ACTION_MOUSE_SCROLL)
        {
            have_action = false;
        }

        vTaskDelay(pdMS_TO_TICKS(USB_HID_SERVICE_PERIOD_MS));
    }
}

// Vendor HID 协议任务串行处理组帧、命令回调和设备到主机报告发送。
static void _protocol_task(void *argument)
{
    struct usb_protocol_report rx_report;
    struct usb_protocol_report tx_report;
    struct usb_protocol_message message;
    struct usb_protocol_parser parser = {0};
    bool have_tx_report = false;
    TickType_t last_report_tick = 0U;
    TickType_t last_drop_log = 0U;
    TickType_t last_error_log = 0U;

    (void)argument;
    usb_protocol_parser_reset(&parser);
    for (;;)
    {
        TickType_t now = xTaskGetTickCount();
        if ((now - last_drop_log) >=
            pdMS_TO_TICKS(USB_PROTOCOL_DROP_LOG_INTERVAL_MS))
        {
            uint32_t dropped = __atomic_exchange_n(&protocol_dropped_count,
                                                   0U,
                                                   __ATOMIC_RELAXED);
            if (dropped > 0U)
            {
                ESP_LOGW(tag, "Dropped %lu vendor HID report(s)",
                         (unsigned long)dropped);
            }
            last_drop_log = now;
        }

        if (__atomic_exchange_n(&protocol_reset_requested,
                                0U,
                                __ATOMIC_ACQ_REL) != 0U)
        {
            usb_protocol_parser_reset(&parser);
            xQueueReset(protocol_rx_queue);
            xQueueReset(protocol_tx_queue);
            have_tx_report = false;
        }

        if (parser.receiving &&
            (now - last_report_tick) >=
                pdMS_TO_TICKS(USB_PROTOCOL_FRAME_TIMEOUT_MS))
        {
            usb_protocol_parser_reset(&parser);
        }

        if (xQueueReceive(protocol_rx_queue,
                          &rx_report,
                          pdMS_TO_TICKS(USB_PROTOCOL_SERVICE_PERIOD_MS)) == pdTRUE)
        {
            bool complete = false;
            last_report_tick = xTaskGetTickCount();
            esp_err_t result = usb_protocol_feed_report(&parser,
                                                        rx_report.data,
                                                        &message,
                                                        &complete);
            if (result != ESP_OK)
            {
                if ((xTaskGetTickCount() - last_error_log) >=
                    pdMS_TO_TICKS(USB_PROTOCOL_DROP_LOG_INTERVAL_MS))
                {
                    ESP_LOGW(tag, "Vendor HID protocol report rejected: %s",
                             esp_err_to_name(result));
                    last_error_log = xTaskGetTickCount();
                }
            }
            else if (complete)
            {
                if (usb_config.heartbeat_callback != NULL &&
                    message.command_code == USB_PROTOCOL_COMMAND_H2D)
                {
                    usb_config.heartbeat_callback(usb_config.heartbeat_context);
                }
                result = usb_protocol_cmd_execute(&message,
                                                  usb_config.time_config_callback,
                                                  usb_config.time_config_context,
                                                  usb_config.agent_index_callback,
                                                  usb_config.agent_index_context,
                                                  usb_config.json_callback,
                                                  usb_config.json_context,
                                                  usb_config.get_device_info_callback,
                                                  usb_config.get_device_info_context,
                                                  usb_config.factory_serial_callback,
                                                  usb_config.factory_serial_context,
                                                  usb_config.key_config_callback,
                                                  usb_config.key_config_context);
                if (result != ESP_OK)
                {
                    if ((xTaskGetTickCount() - last_error_log) >=
                        pdMS_TO_TICKS(USB_PROTOCOL_DROP_LOG_INTERVAL_MS))
                    {
                        ESP_LOGW(tag, "Vendor HID command 0x%02X failed: %s",
                                 message.protocol_id,
                                 esp_err_to_name(result));
                        last_error_log = xTaskGetTickCount();
                    }
                }
                else
                {
                    ESP_LOGI(tag,
                             "Vendor HID command 0x%02X received (%lu-byte payload)",
                             message.protocol_id,
                             (unsigned long)message.data_length);
                }
                usb_protocol_parser_reset(&parser);
            }
        }

        if (!have_tx_report &&
            xQueueReceive(protocol_tx_queue, &tx_report, 0U) == pdTRUE)
        {
            have_tx_report = true;
        }
        if (have_tx_report)
        {
            if (_hid_available() &&
                tud_hid_n_ready(USB_HID_INSTANCE_VENDOR) &&
                tud_hid_n_report(USB_HID_INSTANCE_VENDOR,
                                 0U,
                                 tx_report.data,
                                 USB_PROTOCOL_REPORT_SIZE))
            {
                // 仅在 TinyUSB 接受发送请求后打印，确保日志代表实际排队的 TX Report。
                ESP_LOGI(tag, "Vendor HID TX report (%u bytes)",
                         (unsigned)USB_PROTOCOL_REPORT_SIZE);
                ESP_LOG_BUFFER_HEXDUMP(tag,
                                       tx_report.data,
                                       USB_PROTOCOL_REPORT_SIZE,
                                       ESP_LOG_INFO);
                have_tx_report = false;
            }
        }
    }
}

// TinyUSB Vendor HID 回调只复制完整报告，队列/解析均在协议任务上下文完成。
void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize)
{
    if (instance != USB_HID_INSTANCE_VENDOR ||
        report_id != 0U ||
        report_type != HID_REPORT_TYPE_OUTPUT ||
        buffer == NULL ||
        bufsize != USB_PROTOCOL_REPORT_SIZE)
    {
        return;
    }

    struct usb_protocol_report report = {0};
    memcpy(report.data, buffer, USB_PROTOCOL_REPORT_SIZE);
    if (protocol_rx_queue == NULL ||
        xQueueSend(protocol_rx_queue, &report, 0U) != pdTRUE)
    {
        __atomic_fetch_add(&protocol_dropped_count, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&protocol_reset_requested, 1U, __ATOMIC_RELEASE);
    }
}

// 本阶段不支持控制传输读取 HID 报告，返回 0 触发标准 STALL。
uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t requested_length)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)requested_length;
    return 0U;
}

// CDC 当前仅用于日志输出，主机写入的数据在回调中读空并丢弃。
static void _cdc_rx_callback(int interface_number, cdcacm_event_t *event)
{
    uint8_t buffer[64];
    size_t bytes_read;

    (void)event;
    do
    {
        bytes_read = 0U;
        (void)tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)interface_number,
                                  buffer,
                                  sizeof(buffer),
                                  &bytes_read);
    } while (bytes_read > 0U);
}

#if CONFIG_USB_CDC_AUTO_DOWNLOAD
// 交接 OTG 控制器供 ROM 在复位后保持 USB 连接，强制下次启动进入下载模式并软复位。
// 本函数不返回；成功后设备以 ROM 下载口重新枚举，esptool 无需手动按 BOOT。
static void _enter_download_mode(void)
{
    usb_dc_prepare_persist();
    chip_usb_set_persist_flags(USB_CDC_PERSIST_ENA);
    REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    software_reset_cpu(0);
    // software_reset_cpu 不返回，此处仅为静态分析兜底。
    for (;;)
    {
    }
}

// 识别 esptool 经典复位时序：先 RTS 拉高(EN 复位)使状态机就绪，
// 随后出现 DTR 高且 RTS 低(IO0 进下载)时触发自复位进下载模式。
// 普通终端仅拉 DTR 而从不拉高 RTS，不会误触发；idf.py monitor 的复位除外(见 Kconfig)。
static void _cdc_line_state_callback(int interface_number, cdcacm_event_t *event)
{
    static bool reset_armed = false;
    const bool dtr = event->line_state_changed_data.dtr;
    const bool rts = event->line_state_changed_data.rts;

    (void)interface_number;

    if (rts)
    {
        reset_armed = true;
    }
    else if (reset_armed && dtr)
    {
        reset_armed = false;
        _enter_download_mode(); // 不返回
    }
    else
    {
        reset_armed = false;
    }
}
#endif // CONFIG_USB_CDC_AUTO_DOWNLOAD

// 从日志队列取出文本，在 CDC 已连接且总线可用时发送给主机。
static void _cdc_task(void *argument)
{
    usb_cdc_log_item_t item;

    (void)argument;
    for (;;)
    {
        if (xQueueReceive(cdc_log_queue, &item, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if (!__atomic_load_n(&device_mounted, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&device_suspended, __ATOMIC_ACQUIRE) ||
            !tud_cdc_n_connected(TINYUSB_CDC_ACM_0))
        {
            continue;
        }

        size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                                    (const uint8_t *)item.data,
                                                    item.length);
        if (queued > 0U)
        {
            (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0U);
        }
    }
}

/*
 * ESP 日志输出钩子：先保持原 UART 输出，再尝试把格式化文本非阻塞投递到 CDC 队列。
 * CDC 队列已满时直接丢弃镜像，不影响原始日志路径和实时任务。
 */
static int _log_vprintf(const char *format, va_list arguments)
{
    usb_cdc_log_item_t item = {0};
    va_list uart_arguments;
    va_list usb_arguments;

    va_copy(uart_arguments, arguments);
    int result = uart_vprintf(format, uart_arguments);
    va_end(uart_arguments);

    if (!__atomic_load_n(&log_mirror_enabled, __ATOMIC_ACQUIRE) || cdc_log_queue == NULL)
    {
        return result;
    }

    va_copy(usb_arguments, arguments);
    int length = vsnprintf(item.data, sizeof(item.data), format, usb_arguments);
    va_end(usb_arguments);
    if (length <= 0)
    {
        return result;
    }

    // 单条超长日志截断到固定消息缓冲区，避免在日志路径动态分配内存。
    item.length = (uint16_t)(length < (int)sizeof(item.data) ? length : sizeof(item.data) - 1U);
    (void)xQueueSend(cdc_log_queue, &item, 0U);
    return result;
}

// 将 UAC 组件的数据请求转发给应用注册的麦克风读取回调。
static esp_err_t _uac_input_callback(uint8_t *buffer,
                                     size_t bytes,
                                     size_t *bytes_read,
                                     void *context)
{
    (void)context;
    return usb_config.audio_read_callback(buffer,
                                          bytes,
                                          bytes_read,
                                          usb_config.audio_context);
}

// 使用芯片基础 MAC 生成固定 12 位十六进制 USB 序列号。
static esp_err_t _prepare_serial_string(void)
{
    uint8_t mac[6];

    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_BASE), tag, "Read base MAC failed");
    int length = snprintf(usb_serial_string,
                          sizeof(usb_serial_string),
                          "%02X%02X%02X%02X%02X%02X",
                          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return length == 12 ? ESP_OK : ESP_FAIL;
}

// 创建标准 HID、Vendor HID 协议和 CDC 工作任务；失败时撤销已创建任务。
static esp_err_t _create_worker_tasks(void)
{
    if (xTaskCreate(_hid_task,
                    "usb_hid",
                    USB_HID_TASK_STACK_SIZE,
                    NULL,
                    USB_HID_TASK_PRIORITY,
                    &hid_task_handle) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(_protocol_task,
                    "usb_protocol",
                    USB_PROTOCOL_TASK_STACK_SIZE,
                    NULL,
                    USB_PROTOCOL_TASK_PRIORITY,
                    &protocol_task_handle) != pdPASS)
    {
        vTaskDelete(hid_task_handle);
        hid_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(_cdc_task,
                    "usb_cdc_log",
                    USB_CDC_TASK_STACK_SIZE,
                    NULL,
                    USB_CDC_TASK_PRIORITY,
                    &cdc_task_handle) != pdPASS)
    {
        vTaskDelete(protocol_task_handle);
        protocol_task_handle = NULL;
        vTaskDelete(hid_task_handle);
        hid_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// 删除已创建的 USB 工作任务，可用于部分初始化失败路径。
static void _delete_worker_tasks(void)
{
    if (cdc_task_handle != NULL)
    {
        vTaskDelete(cdc_task_handle);
        cdc_task_handle = NULL;
    }
    if (hid_task_handle != NULL)
    {
        vTaskDelete(hid_task_handle);
        hid_task_handle = NULL;
    }
    if (protocol_task_handle != NULL)
    {
        vTaskDelete(protocol_task_handle);
        protocol_task_handle = NULL;
    }
}

// 删除标准 HID、Vendor HID 协议与 CDC 队列并清空句柄。
static void _delete_queues(void)
{
    if (cdc_log_queue != NULL)
    {
        vQueueDelete(cdc_log_queue);
        cdc_log_queue = NULL;
    }
    if (hid_queue != NULL)
    {
        vQueueDelete(hid_queue);
        hid_queue = NULL;
    }
    if (protocol_tx_queue != NULL)
    {
        vQueueDelete(protocol_tx_queue);
        protocol_tx_queue = NULL;
    }
    if (protocol_rx_queue != NULL)
    {
        vQueueDelete(protocol_rx_queue);
        protocol_rx_queue = NULL;
    }
}

// 按创建顺序的逆序清理由 USB 初始化失败产生的资源。
static void _cleanup_failed_init(bool cdc_initialized, bool tinyusb_installed)
{
    _delete_worker_tasks();
    if (cdc_initialized)
    {
        (void)tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
    }
    if (tinyusb_installed)
    {
        (void)tinyusb_driver_uninstall();
    }
    _delete_queues();
}

/*
 * 初始化 UAC、TinyUSB 高速复合设备、CDC 接口和异步工作任务。
 * 所有服务就绪后才替换日志钩子并标记 USB 可供应用使用。
 */
esp_err_t usb_init(const struct usb_config *config)
{
    esp_err_t ret = ESP_OK;
    bool tinyusb_installed = false;
    bool cdc_initialized = false;

    ESP_RETURN_ON_FALSE(config != NULL &&
                            config->audio_read_callback != NULL,
                        ESP_ERR_INVALID_ARG,
                        tag,
                        "Audio callback is required");
    ESP_RETURN_ON_FALSE(!init_started, ESP_ERR_INVALID_STATE, tag, "USB already initialized");

    // 先锁定初始化入口并创建队列，防止回调在工作资源准备前投递数据。
    init_started = true;
    usb_config = *config;
    hid_queue = xQueueCreate(USB_HID_QUEUE_LENGTH, sizeof(struct usb_hid_action));
    protocol_rx_queue = xQueueCreate(USB_PROTOCOL_RX_QUEUE_LENGTH,
                                     sizeof(struct usb_protocol_report));
    protocol_tx_queue = xQueueCreate(USB_PROTOCOL_TX_QUEUE_LENGTH,
                                     sizeof(struct usb_protocol_report));
    cdc_log_queue = xQueueCreate(USB_CDC_LOG_QUEUE_LENGTH, sizeof(usb_cdc_log_item_t));
    ESP_GOTO_ON_FALSE(hid_queue != NULL && protocol_rx_queue != NULL &&
                          protocol_tx_queue != NULL && cdc_log_queue != NULL,
                      ESP_ERR_NO_MEM,
                      error,
                      tag,
                      "Create USB queues failed");
    ESP_GOTO_ON_ERROR(_prepare_serial_string(), error, tag, "Prepare USB serial failed");

    // UAC 作为复合设备的一部分，不单独初始化 TinyUSB 控制器。
    uac_device_config_t uac_config = {
        .skip_tinyusb_init = true,
        .input_cb = _uac_input_callback,
        .cb_ctx = NULL,
        .spk_itf_num = -1,
        .mic_itf_num = USB_ITF_AUDIO_STREAMING_MIC,
    };
    ESP_GOTO_ON_ERROR(uac_device_init(&uac_config), error, tag,
                      "Initialize UAC part failed");

    // 同时注册全速和高速配置描述符，由主机协商后的总线速度选择。
    tinyusb_config_t tinyusb_config = TINYUSB_CONFIG_HIGH_SPEED(_usb_event_callback, NULL);
    tinyusb_config.descriptor.device = &usb_device_descriptor;
    tinyusb_config.descriptor.qualifier = &usb_device_qualifier;
    tinyusb_config.descriptor.string = usb_string_descriptors;
    tinyusb_config.descriptor.string_count = (int)usb_string_descriptor_count;
    tinyusb_config.descriptor.full_speed_config =
        usb_full_speed_configuration_descriptor;
    tinyusb_config.descriptor.high_speed_config =
        usb_high_speed_configuration_descriptor;
    ESP_GOTO_ON_ERROR(tinyusb_driver_install(&tinyusb_config), error, tag,
                      "Install High-Speed TinyUSB failed");
    tinyusb_installed = true;

    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = _cdc_rx_callback,
#if CONFIG_USB_CDC_AUTO_DOWNLOAD
        .callback_line_state_changed = _cdc_line_state_callback,
#endif
    };
    ESP_GOTO_ON_ERROR(tinyusb_cdcacm_init(&cdc_config), error, tag,
                      "Initialize CDC ACM failed");
    cdc_initialized = true;

    ret = _create_worker_tasks();
    if (ret != ESP_OK)
    {
        ESP_LOGE(tag, "Create USB worker tasks failed: %s", esp_err_to_name(ret));
        goto error;
    }

    // 工作任务创建成功后再启用 CDC 日志镜像，避免日志进入无消费者队列。
    uart_vprintf = vprintf;
    uart_vprintf = esp_log_set_vprintf(_log_vprintf);
    __atomic_store_n(&log_mirror_enabled, true, __ATOMIC_RELEASE);
    __atomic_store_n(&initialized, 1U, __ATOMIC_RELEASE);
    ESP_LOGI(tag, "USB 2.0 HS composite ready: CDC + keyboard/mouse HID + vendor HID + UAC2 microphone, serial=%s",
             usb_serial_string);
    return ESP_OK;

error:
    // 保留 init_started，避免在 USB 栈失败后未经系统重启重复初始化硬件控制器。
    _cleanup_failed_init(cdc_initialized, tinyusb_installed);
    return ret;
}

// 非阻塞投递固定大小的 HID 动作；旧代际动作由工作任务自动丢弃。
static esp_err_t _send_hid_action(struct usb_hid_action *action)
{
    if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) == 0U || !_hid_available())
    {
        return ESP_ERR_INVALID_STATE;
    }

    action->generation = __atomic_load_n(&hid_generation, __ATOMIC_ACQUIRE);
    if (xQueueSend(hid_queue, action, 0U) != pdTRUE)
    {
        __atomic_fetch_add(&hid_dropped_count, 1U, __ATOMIC_RELAXED);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

// 检查调用方读取的会话代际当前是否仍可接受键盘状态。
static bool _keyboard_session_matches(uint32_t expected_generation)
{
    return __atomic_load_n(&hid_generation, __ATOMIC_ACQUIRE) ==
               expected_generation &&
           __atomic_load_n(&device_mounted, __ATOMIC_ACQUIRE) != 0U &&
           __atomic_load_n(&device_suspended, __ATOMIC_ACQUIRE) == 0U &&
           __atomic_load_n(&host_os_locked, __ATOMIC_ACQUIRE) != 0U;
}

// 提交一次完整键盘状态；FIFO 溢出时由 latest desired 保证最终收敛。
esp_err_t usb_send_keyboard(uint32_t expected_generation,
                            const struct usb_keyboard_state *state)
{
    struct usb_hid_action action = {
        .generation = expected_generation,
        .type = USB_HID_ACTION_KEYBOARD,
    };
    bool accepted = false;

    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) == 0U ||
        !_keyboard_session_matches(expected_generation))
    {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&keyboard_state_lock);
    if (_keyboard_session_matches(expected_generation))
    {
        keyboard_state_revision++;
        if (keyboard_state_revision == 0U)
        {
            keyboard_state_revision++;
        }
        desired_keyboard_generation = expected_generation;
        desired_keyboard_revision = keyboard_state_revision;
        desired_keyboard_state = *state;
        action.data.keyboard.revision = keyboard_state_revision;
        action.data.keyboard.state = *state;
        accepted = true;
    }
    portEXIT_CRITICAL(&keyboard_state_lock);

    if (!accepted)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(hid_queue, &action, 0U) != pdTRUE)
    {
        // 中间状态可在过载时合并，latest desired 仍保证最终状态送达。
        __atomic_fetch_add(&hid_dropped_count, 1U, __ATOMIC_RELAXED);
    }
    return _keyboard_session_matches(expected_generation) ?
               ESP_OK : ESP_ERR_INVALID_STATE;
}

// 非阻塞排队一次垂直或水平鼠标滚轮动作。
esp_err_t usb_send_mouse_scroll(int8_t wheel, int8_t pan)
{
    struct usb_hid_action action = {
        .type = USB_HID_ACTION_MOUSE_SCROLL,
        .data.mouse = {
            .wheel = wheel,
            .pan = pan,
        },
    };

    if (wheel == 0 && pan == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return _send_hid_action(&action);
}

// 使当前 HID 动作代际失效，并有限等待工作任务接受取消请求。
esp_err_t usb_hid_cancel(void)
{
    if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) == 0U ||
        hid_task_handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t requested_generation =
        __atomic_add_fetch(&hid_generation, 1U, __ATOMIC_RELEASE);
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(USB_HID_CANCEL_TIMEOUT_MS);

    do
    {
        uint32_t acknowledged = __atomic_load_n(&hid_ack_generation,
                                                 __ATOMIC_ACQUIRE);
        if ((int32_t)(acknowledged - requested_generation) >= 0)
        {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(USB_HID_SERVICE_PERIOD_MS));
    } while ((xTaskGetTickCount() - start) < timeout);

    return ESP_ERR_TIMEOUT;
}

// 一致读取当前 HID 会话；代际变化时重读，避免 OS 与 ready 来自不同会话。
esp_err_t usb_get_hid_session(struct usb_hid_session *session)
{
    uint32_t generation_before;
    uint32_t generation_after;
    uint32_t mounted;
    uint32_t suspended;
    uint32_t os_locked;
    uint32_t host_os;

    if (session == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }

    do
    {
        generation_before = __atomic_load_n(&hid_generation, __ATOMIC_ACQUIRE);
        mounted = __atomic_load_n(&device_mounted, __ATOMIC_ACQUIRE);
        suspended = __atomic_load_n(&device_suspended, __ATOMIC_ACQUIRE);
        os_locked = __atomic_load_n(&host_os_locked, __ATOMIC_ACQUIRE);
        host_os = __atomic_load_n(&current_host_os, __ATOMIC_ACQUIRE);
        generation_after = __atomic_load_n(&hid_generation, __ATOMIC_ACQUIRE);
    } while (generation_before != generation_after);

    session->generation = generation_before;
    session->host_os = (enum usb_host_os)host_os;
    session->ready = mounted != 0U && suspended == 0U && os_locked != 0U;
    return ESP_OK;
}

// 将完整逻辑帧拆为 64 字节报告并一次性加入 Vendor HID 发送队列。
esp_err_t usb_send_protocol_message(const struct usb_protocol_message *message)
{
    uint8_t *frame;
    size_t frame_length = 0U;

    if (message == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) == 0U ||
        !_hid_available())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (message->data_length > USB_PROTOCOL_MAX_PAYLOAD)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    frame_length = USB_PROTOCOL_FRAME_OVERHEAD + message->data_length;
    size_t report_count = (frame_length + USB_PROTOCOL_REPORT_SIZE - 1U) /
                          USB_PROTOCOL_REPORT_SIZE;
    if (uxQueueSpacesAvailable(protocol_tx_queue) < report_count)
    {
        return ESP_ERR_TIMEOUT;
    }

    frame = heap_caps_malloc(frame_length,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame == NULL)
    {
        frame = heap_caps_malloc(frame_length, MALLOC_CAP_8BIT);
    }
    if (frame == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = usb_protocol_encode_frame(message,
                                                 frame,
                                                 frame_length,
                                                 &frame_length);
    if (result != ESP_OK)
    {
        heap_caps_free(frame);
        return result;
    }

    for (size_t offset = 0U; offset < frame_length;
         offset += USB_PROTOCOL_REPORT_SIZE)
    {
        struct usb_protocol_report report = {0};
        size_t chunk_length = frame_length - offset;
        if (chunk_length > USB_PROTOCOL_REPORT_SIZE)
        {
            chunk_length = USB_PROTOCOL_REPORT_SIZE;
        }
        memcpy(report.data, frame + offset, chunk_length);
        if (xQueueSend(protocol_tx_queue, &report, 0U) != pdTRUE)
        {
            __atomic_fetch_add(&protocol_dropped_count, 1U, __ATOMIC_RELAXED);
            __atomic_store_n(&protocol_reset_requested, 1U, __ATOMIC_RELEASE);
            heap_caps_free(frame);
            return ESP_ERR_TIMEOUT;
        }
    }
    heap_caps_free(frame);
    return ESP_OK;
}
