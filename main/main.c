/*
 * 设备主控模块：启动 NVS、Wi-Fi AP/STA、GPIO 面板和 HTTP 服务，
 * 并向网页与 AI 工具提供统一的状态、网络扫描和设备控制接口。
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "chat_ws.h"
#include "device_api.h"
#include "gpio_panel.h"
#include "log_ring.h"

#define AP_SSID "Yao Yao Ling Xian !"
#define AP_PASSWORD "esp32admin"
#define AP_MAX_CLIENTS 4
#define MAX_AP_RECORDS 30
#define MAX_JSON_BODY 1024
#define MAX_LOG_SNAPSHOT 8192
/* A router reboot or an AP channel switch drops the association, and the STA
 * has to bring itself back: without a bounded retry the page keeps reporting
 * "connecting" forever and only a manual click revives the link. */
#define STA_CONNECT_RETRY_MAX 5

static const char *TAG = "device_manager";

/* Both scan buffers live in .bss on purpose.
 *
 * A single wifi_ap_record_t is 92 bytes on the classic ESP32, so a
 * MAX_AP_RECORDS array needs 2760 bytes. The first scan of the session runs
 * from app_main, whose stack is only CONFIG_ESP_MAIN_TASK_STACK_SIZE (3584 B),
 * so keeping that array as a local variable overflows the task stack during
 * boot. File-scope storage removes the risk entirely and costs 2.7 KB of DRAM.
 *
 *   s_scan_scratch -> filled by the Wi-Fi driver
 *   s_ap_records   -> the published snapshot read by the JSON serializer
 */
static wifi_ap_record_t s_ap_records[MAX_AP_RECORDS];
static wifi_ap_record_t s_scan_scratch[MAX_AP_RECORDS];
static uint16_t s_ap_count;
static uint16_t s_total_ap_count;
static uint32_t s_scan_id;

static bool s_sta_connected;
static bool s_sta_configured;
static int s_sta_retry;
static char s_sta_ssid[33];
static char s_sta_ip[16] = "0.0.0.0";
static char s_sta_gw[16] = "0.0.0.0";
static int s_sta_rssi;
/* Last WIFI_EVENT_STA_DISCONNECTED reason.  Without it "STA 连不上" is a
 * dead end: 15 means the password is wrong, 201 means the AP was never heard,
 * 202 means it rejected the credentials outright. */
static int s_sta_reason;

/* Credentials live in their own namespace so a factory reset of the LLM
 * settings does not silently take the network down with it. */
#define NVS_WIFI_NAMESPACE "wifi_cfg"
#define NVS_KEY_WIFI_SSID "ssid"
#define NVS_KEY_WIFI_PASS "password"

extern const unsigned char index_html_start[]
    asm("_binary_index_html_start");
extern const unsigned char index_html_end[]
    asm("_binary_index_html_end");

/* ------------------------------------------------------------------------- */
/* Wi-Fi 辅助函数：扫描、配置持久化与事件状态同步                            */
/* ------------------------------------------------------------------------- */

static const char *auth_mode_to_string(wifi_auth_mode_t auth_mode)
{
    switch (auth_mode) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "UNKNOWN";
    }
}

static const char *reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "POWER-ON";
    case ESP_RST_EXT:
        return "EXTERNAL";
    case ESP_RST_SW:
        return "SOFTWARE";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INT-WDT";
    case ESP_RST_TASK_WDT:
        return "TASK-WDT";
    case ESP_RST_WDT:
        return "WDT";
    case ESP_RST_DEEPSLEEP:
        return "DEEP-SLEEP";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    case ESP_RST_SDIO:
        return "SDIO";
    default:
        return "UNKNOWN";
    }
}

static void mac_to_string(const uint8_t *mac, char *out, size_t out_size)
{
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);
}

static esp_err_t scan_wifi(void)
{
    wifi_scan_config_t scan_config = {
        .show_hidden = true,
    };
    uint16_t record_count = MAX_AP_RECORDS;
    uint16_t total_count = 0;

    ESP_LOGI(TAG, "Scanning nearby Wi-Fi networks...");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_scan_get_ap_num(&total_count);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_scan_get_ap_records(&record_count, s_scan_scratch);
    if (err != ESP_OK) {
        return err;
    }

    /* The driver may report more records than we asked for; never publish more
     * than the array can hold. */
    if (record_count > MAX_AP_RECORDS) {
        record_count = MAX_AP_RECORDS;
    }

    /* Only copy the valid prefix - the tail of s_scan_scratch is stale data
     * from a previous scan. */
    memcpy(s_ap_records, s_scan_scratch,
           (size_t)record_count * sizeof(s_ap_records[0]));
    s_ap_count = record_count;
    s_total_ap_count = total_count;
    s_scan_id++;

    ESP_LOGI(TAG, "Found %" PRIu16 " network(s); saved %" PRIu16 ".",
             total_count, record_count);
    return ESP_OK;
}

/* Turn a 802.11 reason code into something a person can act on.  The numbers
 * on their own are the single least useful part of a failed association. */
static const char *wifi_reason_hint(int reason)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "四次握手超时，通常是密码不对";
    case WIFI_REASON_NO_AP_FOUND:
        return "没扫到这个 AP：确认是 2.4GHz、SSID 没打错、在覆盖范围内";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "AP 的加密方式不支持（WPA3-Enterprise 之类）";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "信号低于阈值，离路由器近一点";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
        return "认证失败，密码不对";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_EXPIRE:
        return "关联被拒：路由器可能开了 MAC 过滤或连接数已满";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "握手超时，信号太弱或路由器响应慢";
    case WIFI_REASON_CONNECTION_FAIL:
        return "连接失败，信号太弱或被 AP 拒绝";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "信标丢失，信号太弱或路由器刚重启";
    case WIFI_REASON_NOT_AUTHED:
    case WIFI_REASON_NOT_ASSOCED:
        return "AP 掉线了（路由器重启或踢掉了本机）";
    default:
        return NULL;
    }
}

static void save_sta_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "unable to store Wi-Fi credentials");
        return;
    }

    esp_err_t err = nvs_set_str(handle, NVS_KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_WIFI_PASS, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    /* Logged explicitly: "did it actually persist?" is otherwise only
     * answerable by power-cycling the board and seeing what happens. */
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi credentials for \"%s\" stored in NVS", ssid);
    } else {
        ESP_LOGE(TAG, "storing Wi-Fi credentials failed: %s",
                 esp_err_to_name(err));
    }
}

static bool load_sta_credentials(char *ssid, size_t ssid_size, char *password,
                                 size_t password_size)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t length = ssid_size;
    bool found = nvs_get_str(handle, NVS_KEY_WIFI_SSID, ssid, &length) ==
                 ESP_OK && ssid[0] != '\0';
    if (found) {
        length = password_size;
        password[0] = '\0';
        nvs_get_str(handle, NVS_KEY_WIFI_PASS, password, &length);
    }
    nvs_close(handle);
    return found;
}

static void clear_sta_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_erase_key(handle, NVS_KEY_WIFI_SSID);
    nvs_erase_key(handle, NVS_KEY_WIFI_PASS);
    nvs_commit(handle);
    nvs_close(handle);
}

/**
 * Point the STA at a network and start connecting.
 *
 * sta.ssid and sta.password are fixed-size fields that are NOT NUL-terminated
 * on the wire: a 32 character SSID or a 64 character passphrase has to survive
 * verbatim, and strlcpy() would silently drop the last byte.  sta_config is
 * zero initialised, so the unused tail is already NUL padded.
 */
static esp_err_t apply_sta_config(const char *ssid, const char *password)
{
    wifi_config_t sta_config = {0};

    size_t ssid_length = strlen(ssid);
    if (ssid_length > sizeof(sta_config.sta.ssid)) {
        ssid_length = sizeof(sta_config.sta.ssid);
    }
    memcpy(sta_config.sta.ssid, ssid, ssid_length);

    size_t password_length = strlen(password);
    if (password_length > sizeof(sta_config.sta.password)) {
        password_length = sizeof(sta_config.sta.password);
    }
    memcpy(sta_config.sta.password, password, password_length);

    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    /* Suppress the auto-reconnect until the new credentials are in place: the
     * disconnect below posts STA_DISCONNECTED, and reconnecting with the old
     * SSID would race the config change. */
    s_sta_configured = false;
    s_sta_retry = 0;
    s_sta_reason = 0;
    esp_wifi_disconnect();

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    s_sta_configured = true;
    return ESP_OK;
}

static void wifi_event_handler(void *argument, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)argument;

    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *event =
                (const wifi_event_sta_connected_t *)event_data;
            s_sta_connected = true;
            s_sta_reason = 0;
            ESP_LOGI(TAG, "STA associated with \"%s\" on channel %u",
                     s_sta_ssid, event != NULL ? event->channel : 0);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *event =
                (const wifi_event_sta_disconnected_t *)event_data;
            s_sta_connected = false;
            s_sta_reason = event != NULL ? (int)event->reason : 0;
            s_sta_rssi = event != NULL ? event->rssi : 0;
            strlcpy(s_sta_ip, "0.0.0.0", sizeof(s_sta_ip));
            strlcpy(s_sta_gw, "0.0.0.0", sizeof(s_sta_gw));

            const char *hint = wifi_reason_hint(s_sta_reason);
            ESP_LOGW(TAG, "STA disconnected from \"%s\": reason %d%s%s",
                     s_sta_ssid, s_sta_reason, hint != NULL ? " - " : "",
                     hint != NULL ? hint : "");

            if (s_sta_configured && s_sta_retry < STA_CONNECT_RETRY_MAX) {
                s_sta_retry++;
                ESP_LOGW(TAG, "reconnect attempt %d/%d", s_sta_retry,
                         STA_CONNECT_RETRY_MAX);
                esp_wifi_connect();
            } else if (s_sta_configured) {
                ESP_LOGE(TAG, "giving up after %d attempts (reason %d)",
                         STA_CONNECT_RETRY_MAX, s_sta_reason);
            }
            break;
        }
        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_sta_ip, sizeof(s_sta_ip));
        esp_ip4addr_ntoa(&event->ip_info.gw, s_sta_gw, sizeof(s_sta_gw));
        s_sta_retry = 0;
        s_sta_reason = 0;
        ESP_LOGI(TAG, "STA acquired IP %s (gw %s)", s_sta_ip, s_sta_gw);
    }
}

static void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* The IDF default country is "01" (world safe), which allows channels 1-11
     * only.  A router sitting on channel 12 or 13 is then simply invisible:
     * the STA reports NO_AP_FOUND (201) no matter how many times the password
     * is retyped.  Widening to 1-13 removes that entire failure class. */
    esp_err_t country_err = esp_wifi_set_country_code("CN", true);
    ESP_LOGI(TAG, "Wi-Fi country CN, channels 1-13 (%s)",
             esp_err_to_name(country_err));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = 0,
            .channel = 1,
            .password = AP_PASSWORD,
            .max_connection = AP_MAX_CLIENTS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Keep the radio awake.  Modem sleep parks the STA in its own DTIM window,
     * which adds a full beacon interval of latency to the WebSocket carrying
     * the chat stream - and a stalled read on that socket is exactly what
     * makes a streaming answer look like it froze.  The board is USB powered,
     * so the idle current is not worth the jitter. */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "Wi-Fi power save disabled for low-latency sockets (%s)",
             esp_err_to_name(ps_err));

    /* The AP channel follows the STA channel in APSTA mode, so anchor the AP
     * to a fixed channel until a station actually connects. */
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi channel %u", primary);
    }
}

/**
 * Rejoin the network saved by the last successful connect.
 *
 * Without this the STA link dies on every power cycle and the chat panel -
 * which needs the board itself to reach the model - stops working until
 * someone re-types the password in the dashboard.
 */
static void connect_saved_sta(void)
{
    char ssid[33] = {0};
    char password[65] = {0};

    if (!load_sta_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "no saved Wi-Fi credentials; connect from the dashboard");
        return;
    }

    esp_err_t err = apply_sta_config(ssid, password);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "rejoining saved network \"%s\"", ssid);
    } else {
        ESP_LOGW(TAG, "rejoin \"%s\" failed: %s", ssid, esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------- */
/* HTTP 辅助函数：限制请求体、统一 JSON 响应和错误格式                       */
/* ------------------------------------------------------------------------- */

static char *read_json_body(httpd_req_t *request)
{
    size_t length = request->content_len;
    if (length == 0 || length > MAX_JSON_BODY) {
        return NULL;
    }

    char *buffer = malloc(length + 1);
    if (buffer == NULL) {
        return NULL;
    }

    size_t received = 0;
    int timeouts = 0;
    while (received < length) {
        int result = httpd_req_recv(request, buffer + received,
                                    length - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Never retry forever. A client that announces a body and then
             * stalls would otherwise pin the single esp_http_server task - and
             * with it the entire dashboard - until the socket finally died. */
            if (++timeouts > 3) {
                free(buffer);
                return NULL;
            }
            continue;
        }
        if (result <= 0) {
            free(buffer);
            return NULL;
        }
        timeouts = 0;
        received += (size_t)result;
    }
    buffer[length] = '\0';
    return buffer;
}

static esp_err_t send_json(httpd_req_t *request, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to serialize JSON");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(request, payload);
    free(payload);
    return result;
}

static esp_err_t send_error_json(httpd_req_t *request, const char *status,
                                 const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        /* httpd_resp_send_err() would force "500 ..." plus a text/plain body,
         * losing the status the caller asked for (400/409/503).  Keep the
         * status even when there is no memory left for a JSON body. */
        return httpd_resp_send_custom_err(request, status, message);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    httpd_resp_set_status(request, status);
    return send_json(request, root);
}

static esp_err_t send_ok_json(httpd_req_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(request, root);
}

/* ------------------------------------------------------------------------- */
/* 状态快照：把芯片、AP、STA、内存和扫描结果序列化为 JSON                    */
/* ------------------------------------------------------------------------- */

static void add_network_list(cJSON *root)
{
    cJSON_AddNumberToObject(root, "total_networks", s_total_ap_count);
    cJSON_AddNumberToObject(root, "scan_id", s_scan_id);

    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    for (uint16_t i = 0; i < s_ap_count; i++) {
        char bssid[18];
        mac_to_string(s_ap_records[i].bssid, bssid, sizeof(bssid));

        cJSON *network = cJSON_CreateObject();
        const char *ssid = s_ap_records[i].ssid[0] != '\0'
                               ? (const char *)s_ap_records[i].ssid
                               : "<hidden>";

        cJSON_AddStringToObject(network, "ssid", ssid);
        cJSON_AddNumberToObject(network, "rssi", s_ap_records[i].rssi);
        cJSON_AddNumberToObject(network, "channel", s_ap_records[i].primary);
        cJSON_AddStringToObject(network, "security",
                                auth_mode_to_string(s_ap_records[i].authmode));
        cJSON_AddStringToObject(network, "bssid", bssid);
        cJSON_AddItemToArray(networks, network);
    }
}

static cJSON *create_status_json(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    wifi_sta_list_t station_list = {0};

    esp_chip_info(&chip_info);
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }
    if (esp_wifi_ap_get_sta_list(&station_list) != ESP_OK) {
        station_list.num = 0;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(device, "cores", chip_info.cores);
    cJSON_AddNumberToObject(device, "revision", chip_info.revision);
    cJSON_AddNumberToObject(device, "flash_bytes", flash_size);
    cJSON_AddNumberToObject(device, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(device, "min_free_heap",
                            esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(device, "uptime_seconds",
                            esp_timer_get_time() / 1000000);
    cJSON_AddStringToObject(device, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(device, "reset_reason",
                            reset_reason_to_string(esp_reset_reason()));

    uint8_t mac[6];
    char mac_text[18];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        mac_to_string(mac, mac_text, sizeof(mac_text));
        cJSON_AddStringToObject(device, "mac_sta", mac_text);
    }
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
        mac_to_string(mac, mac_text, sizeof(mac_text));
        cJSON_AddStringToObject(device, "mac_ap", mac_text);
    }
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        mac_to_string(mac, mac_text, sizeof(mac_text));
        cJSON_AddStringToObject(device, "mac_bt", mac_text);
    }

    cJSON *ap = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddStringToObject(ap, "ssid", AP_SSID);
    cJSON_AddNumberToObject(ap, "clients", station_list.num);
    cJSON_AddNumberToObject(ap, "max_clients", AP_MAX_CLIENTS);
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
        cJSON_AddNumberToObject(ap, "channel", primary);
    }

    cJSON *sta = cJSON_AddObjectToObject(root, "sta");
    cJSON_AddBoolToObject(sta, "connected", s_sta_connected);
    cJSON_AddBoolToObject(sta, "configured", s_sta_configured);
    cJSON_AddStringToObject(sta, "ssid", s_sta_ssid);
    cJSON_AddStringToObject(sta, "ip", s_sta_ip);
    cJSON_AddStringToObject(sta, "gateway", s_sta_gw);
    /* The dashboard needs the raw code *and* the readable form: the code is
     * what you search for, the hint is what tells you to check the password. */
    cJSON_AddNumberToObject(sta, "reason", s_sta_reason);
    const char *reason_hint = wifi_reason_hint(s_sta_reason);
    cJSON_AddStringToObject(sta, "reason_hint",
                            reason_hint != NULL ? reason_hint : "");
    cJSON_AddNumberToObject(sta, "retry", s_sta_retry);
    cJSON_AddNumberToObject(sta, "retry_max", STA_CONNECT_RETRY_MAX);

    if (s_sta_connected) {
        wifi_ap_record_t info = {0};
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            s_sta_rssi = info.rssi;
        }
    }
    cJSON_AddNumberToObject(sta, "rssi", s_sta_rssi);

    add_network_list(root);
    cJSON_AddNumberToObject(root, "log_lines", log_ring_total_lines());

    return root;
}

/* ------------------------------------------------------------------------- */
/* HTTP 路由处理函数                                                         */
/* ------------------------------------------------------------------------- */

static esp_err_t index_handler(httpd_req_t *request)
{
    size_t page_size = (size_t)(index_html_end - index_html_start);

    /* EMBED_TXTFILES appends a NUL terminator that is part of the symbol
     * range. Sending it would advertise a Content-Length one byte longer than
     * the document, so drop it. */
    if (page_size > 0 && index_html_start[page_size - 1] == '\0') {
        page_size--;
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)index_html_start,
                           page_size);
}

static esp_err_t favicon_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    cJSON *root = create_status_json();
    if (root == NULL) {
        return send_error_json(request, "500 Internal Server Error",
                               "Unable to allocate status JSON");
    }
    return send_json(request, root);
}

static esp_err_t scan_handler(httpd_req_t *request)
{
    esp_err_t err = scan_wifi();
    if (err != ESP_OK) {
        return send_error_json(request, "503 Service Unavailable",
                               esp_err_to_name(err));
    }

    cJSON *root = create_status_json();
    if (root == NULL) {
        return send_error_json(request, "500 Internal Server Error", "OOM");
    }
    return send_json(request, root);
}

static esp_err_t gpio_handler(httpd_req_t *request)
{
    char *body = read_json_body(request);
    if (body == NULL) {
        return send_error_json(request, "400 Bad Request", "Missing JSON body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return send_error_json(request, "400 Bad Request", "Malformed JSON");
    }

    const cJSON *gpio_item = cJSON_GetObjectItem(root, "gpio");
    const cJSON *action_item = cJSON_GetObjectItem(root, "action");
    const cJSON *level_item = cJSON_GetObjectItem(root, "level");
    const cJSON *pullup_item = cJSON_GetObjectItem(root, "pullup");

    int gpio = cJSON_IsNumber(gpio_item) ? gpio_item->valueint : -1;
    int level = cJSON_IsNumber(level_item) ? level_item->valueint : 0;
    bool pullup = cJSON_IsTrue(pullup_item);
    char action[16] = {0};
    if (cJSON_IsString(action_item) && action_item->valuestring != NULL) {
        strlcpy(action, action_item->valuestring, sizeof(action));
    }
    cJSON_Delete(root);

    if (gpio < 0) {
        return send_error_json(request, "400 Bad Request", "Missing gpio");
    }

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        return send_error_json(request, "500 Internal Server Error", "OOM");
    }

    esp_err_t err;
    if (strcmp(action, "out") == 0) {
        err = gpio_panel_set_output(gpio, level);
        cJSON_AddStringToObject(response, "action", "out");
        cJSON_AddNumberToObject(response, "level", level ? 1 : 0);
    } else if (strcmp(action, "release") == 0) {
        err = gpio_panel_release(gpio);
        cJSON_AddStringToObject(response, "action", "release");
    } else {
        int read_level = 0;
        err = gpio_panel_read_input(gpio, pullup, &read_level);
        cJSON_AddStringToObject(response, "action", "read");
        cJSON_AddNumberToObject(response, "level", read_level);
    }

    if (err != ESP_OK) {
        cJSON_Delete(response);
        return send_error_json(request, "400 Bad Request",
                               "Pin rejected (6-11 are flash pins, 12/15 are "
                               "strapping pins, 20/24 do not exist, 34-39 are "
                               "input only)");
    }

    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddNumberToObject(response, "gpio", gpio);
    return send_json(request, response);
}

static esp_err_t adc_handler(httpd_req_t *request)
{
    char query[64];
    int channel = 0;
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK) {
        char value[16];
        if (httpd_query_key_value(query, "channel", value, sizeof(value)) ==
            ESP_OK) {
            channel = atoi(value);
        }
    }

    int raw = 0;
    int millivolts = 0;
    esp_err_t err = gpio_panel_read_adc(channel, &raw, &millivolts);
    if (err != ESP_OK) {
        return send_error_json(request, "400 Bad Request",
                               "ADC channel unavailable (use 0-7 on ADC1)");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_error_json(request, "500 Internal Server Error", "OOM");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "channel", channel);
    cJSON_AddNumberToObject(root, "gpio", gpio_panel_adc_gpio(channel));
    cJSON_AddNumberToObject(root, "raw", raw);
    cJSON_AddNumberToObject(root, "millivolts", millivolts);
    cJSON_AddNumberToObject(root, "volts", millivolts / 1000.0);
    return send_json(request, root);
}

static esp_err_t wifi_handler(httpd_req_t *request)
{
    char *body = read_json_body(request);
    if (body == NULL) {
        return send_error_json(request, "400 Bad Request", "Missing JSON body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return send_error_json(request, "400 Bad Request", "Malformed JSON");
    }

    const cJSON *action_item = cJSON_GetObjectItem(root, "action");
    const cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    const cJSON *password_item = cJSON_GetObjectItem(root, "password");

    char action[16] = {0};
    char ssid[33] = {0};
    char password[65] = {0};
    if (cJSON_IsString(action_item) && action_item->valuestring != NULL) {
        strlcpy(action, action_item->valuestring, sizeof(action));
    }
    if (cJSON_IsString(ssid_item) && ssid_item->valuestring != NULL) {
        strlcpy(ssid, ssid_item->valuestring, sizeof(ssid));
    }
    if (cJSON_IsString(password_item) && password_item->valuestring != NULL) {
        strlcpy(password, password_item->valuestring, sizeof(password));
    }
    cJSON_Delete(root);

    if (strcmp(action, "disconnect") == 0) {
        /* Clear the intent *before* the driver posts STA_DISCONNECTED, or the
         * handler's auto-reconnect would fight the user's request. */
        s_sta_configured = false;
        s_sta_retry = 0;
        s_sta_ssid[0] = '\0';
        esp_wifi_disconnect();
        /* An explicit disconnect should survive a reboot, otherwise the board
         * silently rejoins the network the user just left. */
        clear_sta_credentials();
        ESP_LOGI(TAG, "STA disconnect requested");
        return send_ok_json(request);
    }

    if (ssid[0] == '\0') {
        return send_error_json(request, "400 Bad Request", "Missing ssid");
    }

    esp_err_t err = apply_sta_config(ssid, password);
    if (err != ESP_OK) {
        return send_error_json(request, "503 Service Unavailable",
                               esp_err_to_name(err));
    }

    /* Persist, so a power cycle does not lose the network.  The chat panel
     * needs this link to reach the model, and re-typing the password after
     * every reboot would make that feature unusable. */
    save_sta_credentials(ssid, password);

    ESP_LOGI(TAG, "STA connecting to \"%s\"", ssid);
    return send_ok_json(request);
}

static esp_err_t system_handler(httpd_req_t *request)
{
    char *body = read_json_body(request);
    if (body == NULL) {
        return send_error_json(request, "400 Bad Request", "Missing JSON body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return send_error_json(request, "400 Bad Request", "Malformed JSON");
    }

    const cJSON *action_item = cJSON_GetObjectItem(root, "action");
    char action[24] = {0};
    if (cJSON_IsString(action_item) && action_item->valuestring != NULL) {
        strlcpy(action, action_item->valuestring, sizeof(action));
    }
    cJSON_Delete(root);

    if (strcmp(action, "reboot") == 0) {
        ESP_LOGW(TAG, "Reboot requested from the web UI");
        send_ok_json(request);
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
        return ESP_OK;
    }

    if (strcmp(action, "factory_reset") == 0) {
        ESP_LOGW(TAG, "Factory reset requested from the web UI");
        send_ok_json(request);
        vTaskDelay(pdMS_TO_TICKS(400));
        nvs_flash_erase();
        esp_restart();
        return ESP_OK;
    }

    return send_error_json(request, "400 Bad Request", "Unknown action");
}

static esp_err_t logs_handler(httpd_req_t *request)
{
    char *snapshot = malloc(MAX_LOG_SNAPSHOT);
    if (snapshot == NULL) {
        return send_error_json(request, "500 Internal Server Error", "OOM");
    }

    log_ring_snapshot(snapshot, MAX_LOG_SNAPSHOT);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        free(snapshot);
        return send_error_json(request, "500 Internal Server Error", "OOM");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "total_lines", log_ring_total_lines());
    cJSON_AddStringToObject(root, "text", snapshot);
    free(snapshot);

    return send_json(request, root);
}

/* ------------------------------------------------------------------------- */
/* 供聊天工具调用的设备 API                                                  */
/* ------------------------------------------------------------------------- */

esp_err_t device_wifi_scan(void)
{
    return scan_wifi();
}

cJSON *device_network_summary(size_t limit)
{
    cJSON *networks = cJSON_CreateArray();
    if (networks == NULL) {
        return NULL;
    }

    /* s_ap_records is already sorted strongest first by the Wi-Fi driver. */
    for (size_t i = 0; i < s_ap_count && i < limit; i++) {
        cJSON *network = cJSON_CreateObject();
        const char *ssid = s_ap_records[i].ssid[0] != '\0'
                               ? (const char *)s_ap_records[i].ssid
                               : "<hidden>";
        cJSON_AddStringToObject(network, "ssid", ssid);
        cJSON_AddNumberToObject(network, "rssi", s_ap_records[i].rssi);
        cJSON_AddNumberToObject(network, "channel", s_ap_records[i].primary);
        cJSON_AddStringToObject(network, "security",
                                auth_mode_to_string(s_ap_records[i].authmode));
        cJSON_AddItemToArray(networks, network);
    }
    return networks;
}

cJSON *device_status_snapshot(void)
{
    return create_status_json();
}

/* ------------------------------------------------------------------------- */
/* Web 服务初始化与路由注册                                                  */
/* ------------------------------------------------------------------------- */

static httpd_handle_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 10240;
    /* 9 dashboard routes plus the 5 the chat panel adds, against an IDF
     * default of 8. */
    config.max_uri_handlers = 20;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    /* The chat panel keeps a WebSocket open while /api/status is still being
     * polled, so the default pool of 7 sockets is close to the limit. */
    config.max_open_sockets = 7;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler},
        {.uri = "/api/gpio", .method = HTTP_POST, .handler = gpio_handler},
        {.uri = "/api/adc", .method = HTTP_GET, .handler = adc_handler},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_handler},
        {.uri = "/api/system", .method = HTTP_POST, .handler = system_handler},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_handler},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    return server;
}

void app_main(void)
{
    /* Capture logs before anything else: NVS init/erase and the Wi-Fi
     * bring-up are exactly the failures the web log viewer needs to show, and
     * log_ring_init() itself needs neither NVS nor the network. */
    log_ring_init();
    init_nvs();
    ESP_LOGI(TAG, "Booting device dashboard on %s", esp_get_idf_version());

    init_wifi();
    connect_saved_sta();


    esp_err_t gpio_err = gpio_panel_init();
    if (gpio_err != ESP_OK) {
        ESP_LOGW(TAG, "Pin toolbox unavailable: %s", esp_err_to_name(gpio_err));
    }

    esp_err_t scan_result = scan_wifi();
    if (scan_result != ESP_OK) {
        ESP_LOGW(TAG, "Initial scan unavailable; use the web rescan button.");
    }

    httpd_handle_t server = start_web_server();

    /* The chat panel brings up its own worker task; a failure here must not
     * take the dashboard down with it. */
    esp_err_t chat_err = chat_register(server);
    if (chat_err != ESP_OK) {
        ESP_LOGE(TAG, "Chat panel unavailable: %s", esp_err_to_name(chat_err));
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Wi-Fi:    %s", AP_SSID);
    ESP_LOGI(TAG, "Password: %s", AP_PASSWORD);
    ESP_LOGI(TAG, "Dashboard: http://192.168.4.1/");
    ESP_LOGI(TAG, "Chat:      http://192.168.4.1/chat");
    ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned)esp_get_free_heap_size());
    ESP_LOGI(TAG, "========================================");
}
