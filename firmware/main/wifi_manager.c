/*
 * wifi_manager.c — 联网管理：自动连接 + 热点配网（Captive Portal）。
 *
 * 板载 ESP8684 只是 Wi-Fi 网卡，名称/密码/何时连接全部由这里（P4）决定。
 */
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "open_config.h"

#if __has_include("wifi_credentials.h")
#include "wifi_credentials.h"
#endif
#ifndef D50_DEFAULT_WIFI_SSID
#define D50_DEFAULT_WIFI_SSID ""
#define D50_DEFAULT_WIFI_PASSWORD ""
#endif

#define NVS_NAMESPACE        "d50wifi"
#define AP_PASSWORD          "d50setup"
#define AP_IP_TEXT           "192.168.4.1"
#define CONNECT_TIMEOUT_MS   (20000U)
#define SCAN_KEEP            (12U)
#define GOT_IP_BIT           (BIT0)

static const char *tag = "wifi_mgr";

static bool initialized;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static EventGroupHandle_t events;
static SemaphoreHandle_t creds_submitted;
static volatile bool retry_enabled;
static wifi_status_cb_t status_cb;
static char scanned_ssids[SCAN_KEEP][33];
static size_t scanned_count;
static httpd_handle_t http_server;
static TaskHandle_t dns_task;
static volatile bool dns_run;
static int dns_socket = -1;

static void say(const char *line1, const char *line2)
{
    ESP_LOGI(tag, "%s | %s", line1, line2);
    if (status_cb != NULL)
    {
        status_cb(line1, line2);
    }
}

/* ---------- NVS 凭据 ---------- */

static bool load_credentials(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    nvs_handle_t handle;
    bool found = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK)
    {
        size_t ssid_len = ssid_size;
        size_t pass_len = pass_size;
        found = nvs_get_str(handle, "ssid", ssid, &ssid_len) == ESP_OK && ssid[0] != '\0';
        if (found && nvs_get_str(handle, "pass", pass, &pass_len) != ESP_OK)
        {
            pass[0] = '\0';
        }
        nvs_close(handle);
    }
    if (!found && D50_DEFAULT_WIFI_SSID[0] != '\0')
    {
        snprintf(ssid, ssid_size, "%s", D50_DEFAULT_WIFI_SSID);
        snprintf(pass, pass_size, "%s", D50_DEFAULT_WIFI_PASSWORD);
        found = true;
    }
    return found;
}

bool wifi_manager_provision_requested(void)
{
#if D50_WIFI_PORTAL_ENABLE
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        result = nvs_flash_init();
    }
    if (result != ESP_OK)
    {
        return false;
    }
    nvs_handle_t handle;
    bool saved = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK)
    {
        size_t length = 0U;
        saved = nvs_get_str(handle, "ssid", NULL, &length) == ESP_OK && length > 1U;
        nvs_close(handle);
    }
    return !saved;
#else
    return false;
#endif
}

static esp_err_t save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK)
    {
        return result;
    }
    result = nvs_set_str(handle, "ssid", ssid);
    if (result == ESP_OK)
    {
        result = nvs_set_str(handle, "pass", pass);
    }
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

/* ---------- 事件 ---------- */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(events, GOT_IP_BIT);
        if (retry_enabled)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(events, GOT_IP_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    if (initialized)
    {
        return ESP_OK;
    }
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        result = nvs_flash_init();
    }
    if (result != ESP_OK)
    {
        return result;
    }
    events = xEventGroupCreate();
    creds_submitted = xSemaphoreCreateBinary();
    if (events == NULL || creds_submitted == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    result = esp_netif_init();
    if (result != ESP_OK)
    {
        return result;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    {
        return result;
    }
    sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&config);   // 经 esp_wifi_remote 拉起 esp-hosted SPI 链路
    if (result != ESP_OK)
    {
        return result;
    }
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL);
    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK)
    {
        result = esp_wifi_start();
    }
    initialized = result == ESP_OK;
    return result;
}

esp_err_t wifi_manager_scan(uint16_t *count)
{
    esp_err_t result = esp_wifi_scan_start(NULL, true);
    if (result != ESP_OK)
    {
        return result;
    }
    uint16_t found = SCAN_KEEP;
    wifi_ap_record_t records[SCAN_KEEP];
    result = esp_wifi_scan_get_ap_records(&found, records);
    uint16_t total = 0U;
    esp_wifi_scan_get_ap_num(&total);
    if (result == ESP_OK)
    {
        scanned_count = 0U;
        for (uint16_t index = 0U; index < found; index++)
        {
            if (records[index].ssid[0] != '\0')
            {
                snprintf(scanned_ssids[scanned_count++], sizeof(scanned_ssids[0]), "%s",
                         (const char *)records[index].ssid);
            }
        }
    }
    *count = total;
    return result;
}

/* ---------- 配网网页 + DNS 劫持 ---------- */

static void url_decode(char *text)
{
    char *out = text;
    for (const char *in = text; *in != '\0'; in++)
    {
        if (*in == '+')
        {
            *out++ = ' ';
        }
        else if (*in == '%' && in[1] != '\0' && in[2] != '\0')
        {
            char hex[3] = {in[1], in[2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            in += 2;
        }
        else
        {
            *out++ = *in;
        }
    }
    *out = '\0';
}

static bool form_field(const char *body, const char *key, char *value, size_t size)
{
    size_t key_len = strlen(key);
    for (const char *cursor = body; cursor != NULL && *cursor != '\0';)
    {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=')
        {
            const char *start = cursor + key_len + 1;
            const char *end = strchr(start, '&');
            size_t length = end != NULL ? (size_t)(end - start) : strlen(start);
            if (length >= size)
            {
                return false;
            }
            memcpy(value, start, length);
            value[length] = '\0';
            url_decode(value);
            return true;
        }
        cursor = strchr(cursor, '&');
        if (cursor != NULL)
        {
            cursor++;
        }
    }
    return false;
}

static esp_err_t page_handler(httpd_req_t *request)
{
    static const char head[] =
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>D50 Wi-Fi</title><style>body{font-family:sans-serif;background:#222;color:#fff;"
        "max-width:420px;margin:0 auto;padding:24px}input,button{width:100%;box-sizing:border-box;"
        "font-size:18px;padding:12px;margin:8px 0;border-radius:8px;border:0}"
        "button{background:#00c2ff;color:#000;font-weight:bold}</style></head><body>"
        "<h2>D50 Wi-Fi 设置 / Setup</h2>"
        "<form method=post action=/save>"
        "<input name=ssid list=n placeholder='Wi-Fi 名称 / Name' required maxlength=32>"
        "<datalist id=n>";
    static const char tail[] =
        "</datalist><input name=pass type=password placeholder='密码 / Password' maxlength=63>"
        "<button>保存并连接 / Save &amp; connect</button></form>"
        "<p style='color:#999'>仅支持 2.4 GHz 网络。Only 2.4 GHz networks are supported.</p>"
        "</body></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_send_chunk(request, head, HTTPD_RESP_USE_STRLEN);
    for (size_t index = 0U; index < scanned_count; index++)
    {
        // 名称含引号/尖括号/& 的网络不放进候选列表，避免破坏网页（仍可手动输入）。
        if (strpbrk(scanned_ssids[index], "\"<>&'") != NULL)
        {
            continue;
        }
        char option[64];
        snprintf(option, sizeof(option), "<option value=\"%.32s\">", scanned_ssids[index]);
        httpd_resp_send_chunk(request, option, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(request, tail, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t save_handler(httpd_req_t *request)
{
    char body[200] = {0};
    int length = request->content_len < (int)sizeof(body) - 1 ? request->content_len
                                                              : (int)sizeof(body) - 1;
    int received = httpd_req_recv(request, body, (size_t)length);
    char ssid[33] = {0};
    char pass[65] = {0};
    if (received <= 0 || !form_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0')
    {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }
    form_field(body, "pass", pass, sizeof(pass));
    if (save_credentials(ssid, pass) != ESP_OK)
    {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_sendstr(request,
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        "<body style='font-family:sans-serif;background:#222;color:#fff;padding:24px'>"
        "<h2>已保存 / Saved</h2><p>设备正在连接，请看设备屏幕。热点会关闭。</p>"
        "<p>Connecting now. The setup hotspot will close.</p></body>");
    xSemaphoreGive(creds_submitted);
    return ESP_OK;
}

// 其它任何地址（手机的联网探测）都重定向到配网页，触发系统自动弹出配网窗口。
static esp_err_t redirect_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://" AP_IP_TEXT "/");
    return httpd_resp_send(request, NULL, 0);
}

static void dns_task_entry(void *argument)
{
    (void)argument;
    uint8_t packet[256];
    while (dns_run)
    {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int length = recvfrom(dns_socket, packet, sizeof(packet) - 16, 0,
                              (struct sockaddr *)&from, &from_len);
        if (length < 12)
        {
            continue;
        }
        // 跳过问题名，找到问题类型；仅为 A 记录应答 192.168.4.1。
        int offset = 12;
        while (offset < length && packet[offset] != 0)
        {
            offset += packet[offset] + 1;
        }
        offset += 1;
        if (offset + 4 > length)
        {
            continue;
        }
        bool is_a = packet[offset] == 0 && packet[offset + 1] == 1;
        int question_end = offset + 4;
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = is_a ? 1 : 0;
        packet[8] = packet[9] = packet[10] = packet[11] = 0;
        int response_len = question_end;
        if (is_a)
        {
            static const uint8_t answer[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
                                             0x00, 0x3C, 0x00, 0x04, 192, 168, 4, 1};
            memcpy(&packet[response_len], answer, sizeof(answer));
            response_len += sizeof(answer);
        }
        sendto(dns_socket, packet, response_len, 0, (struct sockaddr *)&from, from_len);
    }
    close(dns_socket);
    dns_socket = -1;
    dns_task = NULL;
    vTaskDelete(NULL);
}

static void portal_start(void)
{
    if (ap_netif == NULL)
    {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "D50-Setup-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", AP_PASSWORD);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 6;
    if (httpd_start(&http_server, &config) == ESP_OK)
    {
        const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = page_handler};
        const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler};
        const httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = redirect_handler};
        httpd_register_uri_handler(http_server, &root);
        httpd_register_uri_handler(http_server, &save);
        httpd_register_uri_handler(http_server, &any);
    }

    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_socket >= 0)
    {
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(53),
                                      .sin_addr.s_addr = htonl(INADDR_ANY)};
        struct timeval timeout = {.tv_sec = 1};
        setsockopt(dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        bind(dns_socket, (struct sockaddr *)&address, sizeof(address));
        dns_run = true;
        xTaskCreate(dns_task_entry, "dns", 4096, NULL, 4, &dns_task);
    }
}

static void portal_stop(void)
{
    if (http_server != NULL)
    {
        httpd_stop(http_server);
        http_server = NULL;
    }
    dns_run = false;
    for (int wait = 0; wait < 30 && dns_task != NULL; wait++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
}

/* ---------- 主流程 ---------- */

static bool try_connect(const char *ssid, const char *pass)
{
    wifi_config_t config = {0};
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", ssid);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", pass);
    config.sta.threshold.authmode = pass[0] != '\0' ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &config);
    xEventGroupClearBits(events, GOT_IP_BIT);
    retry_enabled = true;
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if ((bits & GOT_IP_BIT) != 0U)
    {
        return true;   // 保持 retry_enabled：之后掉线自动重连
    }
    retry_enabled = false;
    esp_wifi_disconnect();
    return false;
}

static void manager_task(void *argument)
{
    const bool provision = argument != NULL;
    char ssid[33] = {0};
    char pass[65] = {0};

    if (!provision)
    {
        // 正常开机：有信息就静默连接并一直自动重连，没有就保持离线。
        if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)))
        {
            wifi_config_t config = {0};
            snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", ssid);
            snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", pass);
            config.sta.threshold.authmode = pass[0] != '\0' ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
            esp_wifi_set_config(WIFI_IF_STA, &config);
            retry_enabled = true;
            esp_wifi_connect();
        }
        vTaskDelete(NULL);
        return;
    }

    // 配网模式：开热点等用户提交；连接成功后重启。
    for (;;)
    {
        portal_start();
        char ap_name[33] = {0};
        wifi_config_t ap;
        esp_wifi_get_config(WIFI_IF_AP, &ap);
        snprintf(ap_name, sizeof(ap_name), "%s", (const char *)ap.ap.ssid);
        char password_line[48];
        snprintf(password_line, sizeof(password_line), "密码 %s", AP_PASSWORD);
        say(ap_name, password_line);
        xSemaphoreTake(creds_submitted, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1500));   // 让网页响应先发完
        portal_stop();

        if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)))
        {
            char connecting[48];
            snprintf(connecting, sizeof(connecting), "连接 %.30s ...", ssid);
            say(connecting, "");
            if (try_connect(ssid, pass))
            {
                esp_netif_ip_info_t ip;
                esp_netif_get_ip_info(sta_netif, &ip);
                char address[32];
                snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
                say("已连接", address);
                vTaskDelay(pdMS_TO_TICKS(3000));
                esp_restart();   // 重启回到正常开机，之后自动静默连接
            }
            say("连接失败", "请检查名称和密码");
            vTaskDelay(pdMS_TO_TICKS(2500));
        }
    }
}

void wifi_manager_start(wifi_status_cb_t callback, bool provision)
{
    status_cb = callback;
    if (xTaskCreate(manager_task, "wifi_mgr", 6144, (void *)(uintptr_t)provision, 4, NULL) != pdPASS)
    {
        ESP_LOGE(tag, "cannot start Wi-Fi manager task");
    }
}
