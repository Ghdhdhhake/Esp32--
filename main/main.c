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

#include "ble_scanner.h"
#include "gpio_panel.h"
#include "led_control.h"
#include "log_ring.h"

#define AP_SSID "Yao Yao Ling Xian !"
#define AP_PASSWORD "esp32admin"
#define AP_MAX_CLIENTS 4
#define MAX_AP_RECORDS 30
#define MAX_JSON_BODY 1024
#define MAX_LOG_SNAPSHOT 8192

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
static char s_sta_ssid[33];
static char s_sta_ip[16] = "0.0.0.0";
static char s_sta_gw[16] = "0.0.0.0";
static int s_sta_rssi;

extern const unsigned char index_html_start[]
    asm("_binary_index_html_start");
extern const unsigned char index_html_end[]
    asm("_binary_index_html_end");

/* ------------------------------------------------------------------------- */
/* Wi-Fi helpers                                                             */
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

static void wifi_event_handler(void *argument, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)argument;

    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_CONNECTED:
            s_sta_connected = true;
            ESP_LOGI(TAG, "STA associated with \"%s\"", s_sta_ssid);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_sta_connected = false;
            strlcpy(s_sta_ip, "0.0.0.0", sizeof(s_sta_ip));
            strlcpy(s_sta_gw, "0.0.0.0", sizeof(s_sta_gw));
            s_sta_rssi = 0;
            ESP_LOGW(TAG, "STA disconnected");
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

    /* Wi-Fi power save is the number one cause of a BLE scan that sees
     * nothing: the STA keeps the radio parked in its own sleep window and the
     * coexistence scheduler never hands a slot to the Bluetooth controller.
     * Turning it off costs a little idle current and makes BLE reliable. */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "Wi-Fi power save disabled for BLE coexistence (%s)",
             esp_err_to_name(ps_err));

    /* The AP channel follows the STA channel in APSTA mode, so anchor the AP
     * to a fixed channel until a station actually connects. */
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi channel %u", primary);
    }
}

/* ------------------------------------------------------------------------- */
/* HTTP helpers                                                              */
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
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, message);
        return ESP_FAIL;
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
/* Status                                                                    */
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

    if (s_sta_connected) {
        wifi_ap_record_t info = {0};
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            s_sta_rssi = info.rssi;
        }
    }
    cJSON_AddNumberToObject(sta, "rssi", s_sta_rssi);

    led_state_t led;
    led_control_get_state(&led);
    cJSON *led_json = cJSON_AddObjectToObject(root, "led");
    cJSON_AddNumberToObject(led_json, "gpio", led.gpio);
    cJSON_AddNumberToObject(led_json, "mode", led.mode);
    cJSON_AddNumberToObject(led_json, "brightness", led.brightness);
    cJSON_AddNumberToObject(led_json, "period_ms", led.period_ms);
    cJSON_AddBoolToObject(led_json, "active_high", led.active_high);

    ble_scanner_diag_t diag;
    ble_scanner_get_diag(&diag);
    cJSON *ble = cJSON_AddObjectToObject(root, "ble");
    cJSON_AddBoolToObject(ble, "advertising", diag.advertising);
    cJSON_AddStringToObject(ble, "adv_name", diag.adv_name);
    cJSON_AddBoolToObject(ble, "host_ready", diag.host_ready);
    cJSON_AddNumberToObject(ble, "unique_devices", diag.unique_devices);
    cJSON_AddNumberToObject(ble, "adv_reports", diag.adv_reports);

    add_network_list(root);
    cJSON_AddNumberToObject(root, "log_lines", log_ring_total_lines());

    return root;
}

/* ------------------------------------------------------------------------- */
/* Handlers                                                                  */
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

static esp_err_t led_handler(httpd_req_t *request)
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

    led_state_t current;
    led_control_get_state(&current);

    const cJSON *gpio_item = cJSON_GetObjectItem(root, "gpio");
    const cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
    const cJSON *brightness_item = cJSON_GetObjectItem(root, "brightness");
    const cJSON *period_item = cJSON_GetObjectItem(root, "period_ms");
    const cJSON *polarity_item = cJSON_GetObjectItem(root, "active_high");

    int gpio = cJSON_IsNumber(gpio_item) ? gpio_item->valueint : current.gpio;
    bool active_high = cJSON_IsBool(polarity_item)
                           ? cJSON_IsTrue(polarity_item)
                           : current.active_high;
    int mode = cJSON_IsNumber(mode_item) ? mode_item->valueint
                                         : (int)current.mode;
    int brightness = cJSON_IsNumber(brightness_item)
                         ? brightness_item->valueint
                         : current.brightness;
    int period = cJSON_IsNumber(period_item) ? period_item->valueint
                                             : current.period_ms;

    cJSON_Delete(root);

    if (mode < LED_MODE_OFF || mode > LED_MODE_BREATHE) {
        return send_error_json(request, "400 Bad Request", "Invalid LED mode");
    }

    esp_err_t err = led_control_configure(gpio, active_high);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED GPIO%d rejected: %s", gpio, esp_err_to_name(err));
        return send_error_json(request, "400 Bad Request",
                               "GPIO cannot drive an LED (6-11 are flash pins, "
                               "34-39 are input only)");
    }

    led_control_set_mode((led_mode_t)mode, (uint8_t)brightness,
                         (uint16_t)period);
    led_control_save();

    ESP_LOGI(TAG, "LED -> GPIO%d mode=%d brightness=%d%% period=%dms %s", gpio,
             mode, brightness, period,
             active_high ? "active-high" : "active-low");

    return send_ok_json(request);
}

static esp_err_t ble_status_handler(httpd_req_t *request)
{
    /* 40 devices x 56 bytes = 2240 bytes, which does not belong on the HTTP
     * task stack. esp_http_server serves every request from a single task, so
     * a file-scope buffer is safe here and avoids a per-poll heap allocation
     * (the UI polls this endpoint every 700 ms while a scan is running). */
    static ble_scanner_device_t devices[BLE_SCANNER_MAX_DEVICES];
    size_t device_count =
        ble_scanner_get_devices(devices, BLE_SCANNER_MAX_DEVICES);
    ble_scanner_diag_t diag;
    ble_scanner_get_diag(&diag);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_error_json(request, "500 Internal Server Error", "OOM");
    }

    cJSON_AddNumberToObject(root, "ble_count", device_count);
    cJSON_AddNumberToObject(root, "ble_scan_id", ble_scanner_get_scan_id());
    cJSON_AddBoolToObject(root, "advertising", diag.advertising);
    cJSON_AddStringToObject(root, "adv_name", diag.adv_name);

    static const char *state_names[] = {"idle", "running", "done", "error"};
    const char *state_name = "idle";
    if (diag.state <= BLE_SCAN_ERROR) {
        state_name = state_names[diag.state];
    }
    cJSON_AddStringToObject(root, "scan_state", state_name);

    cJSON *diagnostics = cJSON_AddObjectToObject(root, "diag");
    cJSON_AddBoolToObject(diagnostics, "host_ready", diag.host_ready);
    cJSON_AddBoolToObject(diagnostics, "controller_ok", diag.controller_ok);
    cJSON_AddNumberToObject(diagnostics, "controller_status",
                            diag.controller_status);
    cJSON_AddNumberToObject(diagnostics, "last_sync_rc", diag.last_sync_rc);
    cJSON_AddNumberToObject(diagnostics, "last_scan_rc", diag.last_scan_rc);
    cJSON_AddNumberToObject(diagnostics, "last_adv_rc", diag.last_adv_rc);
    cJSON_AddNumberToObject(diagnostics, "host_resets", diag.host_resets);
    cJSON_AddNumberToObject(diagnostics, "adv_reports", diag.adv_reports);
    cJSON_AddNumberToObject(diagnostics, "adv_reports_scan",
                            diag.adv_reports_scan);
    cJSON_AddNumberToObject(diagnostics, "scan_runs", diag.scan_runs);
    cJSON_AddNumberToObject(diagnostics, "scan_timeouts", diag.scan_timeouts);
    cJSON_AddNumberToObject(diagnostics, "adv_starts", diag.adv_starts);
    cJSON_AddNumberToObject(diagnostics, "scan_duration_ms",
                            diag.scan_duration_ms);
    cJSON_AddNumberToObject(diagnostics, "scan_elapsed_ms",
                            diag.scan_elapsed_ms);
    if (diag.own_address_valid) {
        char text[18];
        snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                 diag.own_address[5], diag.own_address[4], diag.own_address[3],
                 diag.own_address[2], diag.own_address[1], diag.own_address[0]);
        cJSON_AddStringToObject(diagnostics, "own_address", text);
        cJSON_AddStringToObject(
            diagnostics, "own_address_type",
            ble_scanner_address_type_name(diag.own_address_type));
    }

    cJSON *device_array = cJSON_AddArrayToObject(root, "ble_devices");
    for (size_t i = 0; i < device_count; i++) {
        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "name", devices[i].name);
        cJSON_AddStringToObject(device, "address", devices[i].address);
        cJSON_AddNumberToObject(device, "rssi", devices[i].rssi);
        cJSON_AddNumberToObject(device, "seen", devices[i].seen_count);
        cJSON_AddStringToObject(
            device, "address_type",
            ble_scanner_address_type_name(devices[i].address_type));
        cJSON_AddItemToArray(device_array, device);
    }

    return send_json(request, root);
}

static esp_err_t ble_scan_start_handler(httpd_req_t *request)
{
    uint32_t duration_ms = BLE_SCANNER_DEFAULT_SCAN_MS;

    char *body = read_json_body(request);
    if (body != NULL) {
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (root != NULL) {
            const cJSON *duration = cJSON_GetObjectItem(root, "duration_ms");
            if (cJSON_IsNumber(duration) && duration->valueint > 0) {
                duration_ms = (uint32_t)duration->valueint;
            }
            cJSON_Delete(root);
        }
    }

    esp_err_t err = ble_scanner_start_scan(duration_ms);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error_json(request, "409 Conflict",
                               "BLE is not ready or a scan is already running");
    }
    if (err != ESP_OK) {
        return send_error_json(request, "503 Service Unavailable",
                               esp_err_to_name(err));
    }

    return send_ok_json(request);
}

static esp_err_t ble_scan_stop_handler(httpd_req_t *request)
{
    esp_err_t err = ble_scanner_stop_scan();
    if (err != ESP_OK) {
        return send_error_json(request, "503 Service Unavailable",
                               esp_err_to_name(err));
    }
    return send_ok_json(request);
}

static esp_err_t ble_adv_handler(httpd_req_t *request)
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

    const cJSON *enabled_item = cJSON_GetObjectItem(root, "enabled");
    const cJSON *name_item = cJSON_GetObjectItem(root, "name");
    bool enabled = cJSON_IsTrue(enabled_item);
    char name[BLE_SCANNER_ADV_NAME_SIZE] = {0};
    if (cJSON_IsString(name_item) && name_item->valuestring != NULL) {
        strlcpy(name, name_item->valuestring, sizeof(name));
    }
    cJSON_Delete(root);

    esp_err_t err;
    if (enabled) {
        err = ble_scanner_advertising_start(name[0] != '\0' ? name : NULL);
    } else {
        err = ble_scanner_advertising_stop();
    }

    if (err != ESP_OK) {
        return send_error_json(request, "503 Service Unavailable",
                               "BLE advertising request failed");
    }
    return send_ok_json(request);
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
                               "Pin rejected (6-11 are flash pins, 20/24 do "
                               "not exist, 34-39 are input only)");
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
        esp_wifi_disconnect();
        s_sta_configured = false;
        s_sta_ssid[0] = '\0';
        ESP_LOGI(TAG, "STA disconnect requested");
        return send_ok_json(request);
    }

    if (ssid[0] == '\0') {
        return send_error_json(request, "400 Bad Request", "Missing ssid");
    }

    /* sta.ssid and sta.password are fixed-size fields that are NOT
     * NUL-terminated on the wire: a 32 character SSID or a 64 character
     * passphrase has to survive verbatim, and strlcpy() would silently drop
     * the last byte. sta_config is zero initialised, so the unused tail is
     * already NUL padded. */
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

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        return send_error_json(request, "400 Bad Request",
                               esp_err_to_name(err));
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        return send_error_json(request, "503 Service Unavailable",
                               esp_err_to_name(err));
    }

    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    s_sta_configured = true;
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
/* Server                                                                    */
/* ------------------------------------------------------------------------- */

static httpd_handle_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 10240;
    /* 13 route handlers are registered below, well above the IDF default of 8. */
    config.max_uri_handlers = 20;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler},
        {.uri = "/api/led", .method = HTTP_POST, .handler = led_handler},
        {.uri = "/api/ble/status",
         .method = HTTP_GET,
         .handler = ble_status_handler},
        {.uri = "/api/ble/scan/start",
         .method = HTTP_POST,
         .handler = ble_scan_start_handler},
        {.uri = "/api/ble/scan/stop",
         .method = HTTP_POST,
         .handler = ble_scan_stop_handler},
        {.uri = "/api/ble/adv", .method = HTTP_POST, .handler = ble_adv_handler},
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
    init_nvs();
    log_ring_init();
    ESP_LOGI(TAG, "Booting device dashboard on %s", esp_get_idf_version());

    init_wifi();

    esp_err_t led_err = led_control_init();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "LED driver unavailable: %s", esp_err_to_name(led_err));
    }

    esp_err_t gpio_err = gpio_panel_init();
    if (gpio_err != ESP_OK) {
        ESP_LOGW(TAG, "Pin toolbox unavailable: %s", esp_err_to_name(gpio_err));
    }

    /* BLE failures must not kill the dashboard - the whole point is to be able
     * to read the reason from the web page. */
    esp_err_t ble_err = ble_scanner_init();
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE scanner unavailable: %s - the BLE panel will report "
                      "the failure code",
                 esp_err_to_name(ble_err));
    }

    esp_err_t scan_result = scan_wifi();
    if (scan_result != ESP_OK) {
        ESP_LOGW(TAG, "Initial scan unavailable; use the web rescan button.");
    }

    start_web_server();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Wi-Fi:    %s", AP_SSID);
    ESP_LOGI(TAG, "Password: %s", AP_PASSWORD);
    ESP_LOGI(TAG, "Open:     http://192.168.4.1");
    ESP_LOGI(TAG, "========================================");
}
