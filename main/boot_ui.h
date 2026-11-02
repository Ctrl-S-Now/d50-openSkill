#pragma once
#include <stdbool.h>

#include "esp_err.h"

/* 自检条目。 */
enum boot_item
{
    BOOT_ITEM_DISPLAY,
    BOOT_ITEM_LED,
    BOOT_ITEM_PSRAM,
    BOOT_ITEM_SPEAKER,
    BOOT_ITEM_MIC,
    BOOT_ITEM_INPUT,
    BOOT_ITEM_USB,
    BOOT_ITEM_WIFI,
    BOOT_ITEM_COUNT,
};

/*
 * 开机四屏，每屏只讲一件事：
 *   1 标志（淡入淡出） → 2 欢迎语 → 3 自检清单 → 4 首页（二维码 + 总结 + 联网状态）
 * 渐亮渐暗全部用背光 PWM 完成（app_main 里的 transition）；show 函数只负责在全黑时换内容。
 */
esp_err_t boot_ui_show_logo(void);

esp_err_t boot_ui_show_welcome(void);
esp_err_t boot_ui_show_selftest(void);
/* 更新一条自检结果；pending 为真时显示“···”。 */
void boot_ui_set_item(enum boot_item item, bool pending, bool ok, const char *detail);
/* ok[i] 为各项自检结果，用于首页的一句话总结。 */
esp_err_t boot_ui_show_home(const bool ok[BOOT_ITEM_COUNT]);

/* Wi-Fi 配网专用页（D50_WIFI_PORTAL_ENABLE=1 且设备里没有 Wi-Fi 时进入）。line1/line2 随配网进度更新。 */
esp_err_t boot_ui_show_provision(void);
void boot_ui_set_provision(const char *line1, const char *line2);
