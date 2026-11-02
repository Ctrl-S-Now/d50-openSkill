idf_component_register(
    SRCS
        "app_main.c"
        "boot_ui.c"
        "selftest.c"
        "sound.c"
        "led_fx.c"
        "dino_game.c"
        "wifi_manager.c"
        "font_asset.c"
        "ui_font_bin.c"
        "ui_image_boot_logo.c"
        "ui_font_noto_sans_sc_bold_24.c"
    INCLUDE_DIRS "."
    EMBED_FILES "sounds/boot.wav" "sounds/click.wav"
    PRIV_REQUIRES
        bsp
        usb
        lvgl__lvgl
        esp_partition
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_psram
        esp_http_server
        lwip
        heap
)

# LVGL 默认字体使用 main 内置的 Noto Sans SC Bold 24 px 子集（ASCII + 少量符号），
# 汉字等其余文字走 font_assets 分区里的完整 24 px 字库（见 font_asset.c）。
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
set(LVGL_BUILTIN_FONT_OPTIONS
    LV_FONT_MONTSERRAT_8  LV_FONT_MONTSERRAT_10 LV_FONT_MONTSERRAT_12
    LV_FONT_MONTSERRAT_14 LV_FONT_MONTSERRAT_16 LV_FONT_MONTSERRAT_18
    LV_FONT_MONTSERRAT_20 LV_FONT_MONTSERRAT_22 LV_FONT_MONTSERRAT_24
    LV_FONT_MONTSERRAT_26 LV_FONT_MONTSERRAT_28 LV_FONT_MONTSERRAT_30
    LV_FONT_MONTSERRAT_32 LV_FONT_MONTSERRAT_34 LV_FONT_MONTSERRAT_36
    LV_FONT_MONTSERRAT_38 LV_FONT_MONTSERRAT_40 LV_FONT_MONTSERRAT_42
    LV_FONT_MONTSERRAT_44 LV_FONT_MONTSERRAT_46 LV_FONT_MONTSERRAT_48
    LV_FONT_MONTSERRAT_28_COMPRESSED
    LV_FONT_DEJAVU_16_PERSIAN_HEBREW
    LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    LV_FONT_UNSCII_8
    LV_FONT_UNSCII_16
)
foreach(font_option IN LISTS LVGL_BUILTIN_FONT_OPTIONS)
    target_compile_definitions(${lvgl_lib} PUBLIC "${font_option}=0")
endforeach()
target_compile_definitions(${lvgl_lib} PUBLIC
    "LV_FONT_DEFAULT=&ui_font_noto_sans_sc_bold_24"
    "LV_FONT_CUSTOM_DECLARE=LV_FONT_DECLARE(ui_font_noto_sans_sc_bold_24)"
    LV_USE_FONT_COMPRESSED=1
)

# 完整 24 px 字库烧到 font_assets 分区。
set(FONT_ASSET_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/noto_sans_sc_24.bin")
set(FONT_ASSET_IMAGE "${CMAKE_BINARY_DIR}/font_assets.bin")
add_custom_command(
    OUTPUT "${FONT_ASSET_IMAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONT_ASSET_SOURCE}" "${FONT_ASSET_IMAGE}"
    DEPENDS "${FONT_ASSET_SOURCE}"
    COMMENT "Prepare 24 px font asset"
    VERBATIM
)
add_custom_target(font_assets_bin ALL DEPENDS "${FONT_ASSET_IMAGE}")
esptool_py_flash_to_partition(flash font_assets "${FONT_ASSET_IMAGE}")
add_dependencies(flash font_assets_bin)
