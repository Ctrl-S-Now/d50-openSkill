idf_component_register(
    SRCS
        "usb_device.c"
        "usb_descriptors.c"
        "usb_protocol.c"
        "usb_protocol_cmd.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    PRIV_REQUIRES
        espressif__esp_tinyusb
        espressif__usb_device_uac
        esp_hal_usb
        esp_hw_support
        esp_system
        soc
        heap
        freertos
        log
)

idf_component_get_property(TUSB_LIB espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(ESP_TINYUSB_LIB espressif__esp_tinyusb COMPONENT_LIB)
idf_component_get_property(ESP_TINYUSB_DIR espressif__esp_tinyusb COMPONENT_DIR)
idf_component_get_property(UAC_LIB espressif__usb_device_uac COMPONENT_LIB)
idf_component_get_property(UAC_DIR espressif__usb_device_uac COMPONENT_DIR)

set(USB_CONFIG_DIR "${COMPONENT_DIR}/private_include")
set(UAC_CONFIG_DIR "${UAC_DIR}/tusb_uac")
set(ESP_TINYUSB_CONFIG_DIR "${ESP_TINYUSB_DIR}/include")

target_include_directories(${TUSB_LIB} BEFORE PRIVATE
    "${USB_CONFIG_DIR}"
    "${UAC_CONFIG_DIR}"
)
target_include_directories(${ESP_TINYUSB_LIB} BEFORE PRIVATE
    "${USB_CONFIG_DIR}"
    "${UAC_CONFIG_DIR}"
)
target_include_directories(${UAC_LIB} BEFORE PRIVATE
    "${USB_CONFIG_DIR}"
    "${ESP_TINYUSB_CONFIG_DIR}"
    "${UAC_CONFIG_DIR}"
)
target_include_directories(${COMPONENT_LIB} PRIVATE "${UAC_CONFIG_DIR}")

# 观察真实 DCD SETUP/Bus Reset 事件，用于主机 OS 枚举指纹和 HID 会话失效。
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=dcd_event_handler"
)
