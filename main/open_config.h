/*
 * open_config.h — 开源固件可自定义项（改这里即可）。
 */
#pragma once

/* 开源社区地址：屏幕底部显示文字，并生成二维码。 */
#define D50_COMMUNITY_URL        "https://www.ulanzistudio.com"
#define D50_COMMUNITY_URL_TEXT   "www.ulanzistudio.com"

/* 没有 Wi-Fi 信息时是否进入"热点配网页"（专用页，显示热点名和密码；配好后设备自动重启）。
 * 默认关闭，开机界面里没有任何联网提示；用户不想提供 Wi-Fi 密码时改为 1 并重新烧录。 */
#define D50_WIFI_PORTAL_ENABLE   0
