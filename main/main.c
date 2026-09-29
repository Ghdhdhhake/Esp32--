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
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "ble_scanner.h"

#define AP_SSID "Yao Yao Ling Xian !"
#define AP_PASSWORD "esp32admin"
#define AP_MAX_CLIENTS 4
#define MAX_AP_RECORDS 20

static const char *TAG = "device_manager";

static wifi_ap_record_t s_ap_records[MAX_AP_RECORDS];
static uint16_t s_ap_count;
static uint16_t s_total_ap_count;
static uint32_t s_scan_id;

extern const unsigned char index_html_start[]
    asm("_binary_index_html_start");
extern const unsigned char index_html_end[]
    asm("_binary_index_html_end");

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
        return "WPA2-ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3-PSK";
    default:
        return "UNKNOWN";
    }
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
    wifi_ap_record_t records[MAX_AP_RECORDS] = {0};
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

    err = esp_wifi_scan_get_ap_records(&record_count, records);
    if (err != ESP_OK) {
        return err;
    }

    memcpy(s_ap_records, records, sizeof(records));
    s_ap_count = record_count;
    s_total_ap_count = total_count;
    s_scan_id++;

    ESP_LOGI(TAG, "Found %" PRIu16 " network(s); saved %" PRIu16 ".",
             total_count, record_count);
    return ESP_OK;
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

    cJSON_AddStringToObject(root, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(root, "cores", chip_info.cores);
    cJSON_AddNumberToObject(root, "revision", chip_info.revision);
    cJSON_AddNumberToObject(root, "flash_bytes", flash_size);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "clients", station_list.num);
    cJSON_AddNumberToObject(root, "total_networks", s_total_ap_count);
    cJSON_AddNumberToObject(root, "scan_id", s_scan_id);

    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    for (uint16_t i = 0; i < s_ap_count; i++) {
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_ap_records[i].bssid[0], s_ap_records[i].bssid[1],
                 s_ap_records[i].bssid[2], s_ap_records[i].bssid[3],
                 s_ap_records[i].bssid[4], s_ap_records[i].bssid[5]);

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

    return root;
}

static esp_err_t send_status_json(httpd_req_t *request)
{
    cJSON *root = create_status_json();
    if (root == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to allocate status JSON");
        return ESP_ERR_NO_MEM;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to serialize status JSON");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(request, payload);
    free(payload);
    return result;
}

static esp_err_t index_handler(httpd_req_t *request)
{
    size_t page_size = index_html_end - index_html_start;
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)index_html_start,
                           page_size);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    return send_status_json(request);
}

static esp_err_t scan_handler(httpd_req_t *request)
{
    esp_err_t err = scan_wifi();
    if (err != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return err;
    }

    return send_status_json(request);
}

static esp_err_t send_ble_json(httpd_req_t *request)
{
    ble_scanner_device_t devices[BLE_SCANNER_MAX_DEVICES];
    size_t device_count = ble_scanner_get_devices(
        devices, BLE_SCANNER_MAX_DEVICES);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to allocate BLE JSON");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "ble_count", device_count);
    cJSON_AddNumberToObject(root, "ble_scan_id", ble_scanner_get_scan_id());
    cJSON *device_array = cJSON_AddArrayToObject(root, "ble_devices");

    for (size_t i = 0; i < device_count; i++) {
        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "name", devices[i].name);
        cJSON_AddStringToObject(device, "address", devices[i].address);
        cJSON_AddNumberToObject(device, "rssi", devices[i].rssi);
        cJSON_AddStringToObject(
            device, "address_type",
            ble_scanner_address_type_name(devices[i].address_type));
        cJSON_AddItemToArray(device_array, device);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to serialize BLE JSON");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(request, payload);
    free(payload);
    return result;
}

static esp_err_t ble_scan_handler(httpd_req_t *request)
{
    ESP_LOGI(TAG, "Web client requested a 5-second BLE scan");
    esp_err_t err = ble_scanner_scan(5000);
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(request, esp_err_to_name(err));
    }

    return send_ble_json(request);
}

static httpd_handle_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    const httpd_uri_t scan_uri = {
        .uri = "/api/scan",
        .method = HTTP_GET,
        .handler = scan_handler,
    };
    const httpd_uri_t ble_scan_uri = {
        .uri = "/api/ble/scan",
        .method = HTTP_GET,
        .handler = ble_scan_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &scan_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ble_scan_uri));
    return server;
}

void app_main(void)
{
    init_nvs();
    init_wifi();
    ESP_ERROR_CHECK(ble_scanner_init());

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
