#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 配网进度回调（两行文字，在 Wi-Fi 任务里调用），仅配网模式使用。 */
typedef void (*wifi_status_cb_t)(const char *line1, const char *line2);

/* 初始化协处理器链路并启动 Wi-Fi（STA 模式）。可重复调用。 */
esp_err_t wifi_manager_init(void);
/* 阻塞扫描一次，返回附近网络数量（并记住网络名，供配网网页选择）。 */
esp_err_t wifi_manager_scan(uint16_t *count);
/*
 * 是否需要进入热点配网：open_config.h 里 D50_WIFI_PORTAL_ENABLE 为 1，
 * 且设备闪存里还没有保存过 Wi-Fi。默认（未定义）恒为 false。
 */
bool wifi_manager_provision_requested(void);

/*
 * 启动联网任务。
 *   provision = false（正常开机）：用 NVS 里保存的 Wi-Fi，没有则用 wifi_credentials.h 的默认值，
 *       静默连接并在掉线后自动重连；两者都没有就保持离线，不打扰用户。
 *   provision = true（配网模式）：开热点 D50-Setup-xxxx（密码 d50setup）+ 配网网页，
 *       用户提交后保存到 NVS 并连接；成功后重启，下次开机自动沿用。
 */
void wifi_manager_start(wifi_status_cb_t status_cb, bool provision);
