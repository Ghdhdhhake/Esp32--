#include "ble_scanner.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "esp_log.h"

static const char *TAG = "ble_scanner";

static ble_scanner_device_t s_devices[BLE_SCANNER_MAX_DEVICES];
static size_t s_device_count;
static uint32_t s_scan_id;
static bool s_ready;
static bool s_scanning;
static SemaphoreHandle_t s_devices_mutex;
static SemaphoreHandle_t s_scan_done;
static SemaphoreHandle_t s_sync_done;

static void copy_safe_name(char *destination, size_t destination_size,
                           const uint8_t *source, size_t source_size)
{
    size_t length = source_size;
    if (length >= destination_size) {
        length = destination_size - 1;
    }

    for (size_t i = 0; i < length; i++) {
        uint8_t value = source[i];
        destination[i] = value >= 32 && value <= 126 ? (char)value : '?';
    }
    destination[length] = '\0';
}

static void format_address(char *destination, size_t destination_size,
                           const ble_addr_t *address)
{
    snprintf(destination, destination_size,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             address->val[5], address->val[4], address->val[3],
             address->val[2], address->val[1], address->val[0]);
}

static void save_advertisement(const struct ble_gap_disc_desc *discovery)
{
    struct ble_hs_adv_fields fields = {0};
    bool fields_valid = ble_hs_adv_parse_fields(
                            &fields, discovery->data,
                            discovery->length_data) == 0;
    char address[18];
    format_address(address, sizeof(address), &discovery->addr);

    xSemaphoreTake(s_devices_mutex, portMAX_DELAY);

    size_t index = s_device_count;
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].address, address) == 0) {
            index = i;
            break;
        }
    }

    if (index == s_device_count) {
        if (s_device_count >= BLE_SCANNER_MAX_DEVICES) {
            xSemaphoreGive(s_devices_mutex);
            return;
        }

        memset(&s_devices[index], 0, sizeof(s_devices[index]));
        snprintf(s_devices[index].name, sizeof(s_devices[index].name),
                 "Unknown");
        snprintf(s_devices[index].address, sizeof(s_devices[index].address),
                 "%s", address);
        s_devices[index].address_type = discovery->addr.type;
        s_device_count++;
    }

    s_devices[index].rssi = discovery->rssi;
    if (fields_valid && fields.name != NULL && fields.name_len > 0) {
        copy_safe_name(s_devices[index].name, sizeof(s_devices[index].name),
                       fields.name, fields.name_len);
    }

    xSemaphoreGive(s_devices_mutex);
}

static int gap_event_handler(struct ble_gap_event *event, void *argument)
{
    (void)argument;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        save_advertisement(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        xSemaphoreGive(s_scan_done);
        ESP_LOGI(TAG, "BLE scan complete; %u device(s)",
                 (unsigned)s_device_count);
        return 0;

    default:
        return 0;
    }
}

static void host_reset_callback(int reason)
{
    s_ready = false;
    ESP_LOGE(TAG, "NimBLE host reset; reason=%d", reason);
}

static void host_sync_callback(void)
{
    int result = ble_hs_util_ensure_addr(0);
    if (result != 0) {
        ESP_LOGE(TAG, "Unable to set BLE identity address; rc=%d", result);
        return;
    }

    s_ready = true;
    xSemaphoreGive(s_sync_done);
    ESP_LOGI(TAG, "NimBLE observer ready");
}

static void host_task(void *parameter)
{
    (void)parameter;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_scanner_init(void)
{
    s_devices_mutex = xSemaphoreCreateMutex();
    s_scan_done = xSemaphoreCreateBinary();
    s_sync_done = xSemaphoreCreateBinary();
    if (s_devices_mutex == NULL || s_scan_done == NULL ||
        s_sync_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s",
                 esp_err_to_name(result));
        return result;
    }

    ble_hs_cfg.reset_cb = host_reset_callback;
    ble_hs_cfg.sync_cb = host_sync_callback;
    nimble_port_freertos_init(host_task);

    if (xSemaphoreTake(s_sync_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for NimBLE host sync");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t ble_scanner_scan(uint32_t duration_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scanning) {
        return ESP_ERR_INVALID_STATE;
    }

    while (xSemaphoreTake(s_scan_done, 0) == pdTRUE) {
    }

    xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    xSemaphoreGive(s_devices_mutex);

    uint8_t own_address_type;
    int result = ble_hs_id_infer_auto(0, &own_address_type);
    if (result != 0) {
        ESP_LOGE(TAG, "Unable to infer BLE address type; rc=%d", result);
        return ESP_FAIL;
    }

    struct ble_gap_disc_params parameters = {
        .filter_duplicates = 0,
        .passive = 0,
        .itvl = 0,
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
    };

    s_scanning = true;
    result = ble_gap_disc(own_address_type, duration_ms, &parameters,
                          gap_event_handler, NULL);
    if (result != 0) {
        s_scanning = false;
        ESP_LOGE(TAG, "Unable to start BLE scan; rc=%d", result);
        return ESP_FAIL;
    }

    TickType_t wait_time = pdMS_TO_TICKS(duration_ms + 1500);
    if (xSemaphoreTake(s_scan_done, wait_time) != pdTRUE) {
        ble_gap_disc_cancel();
        s_scanning = false;
        return ESP_ERR_TIMEOUT;
    }

    s_scan_id++;
    return ESP_OK;
}

size_t ble_scanner_get_devices(ble_scanner_device_t *devices,
                               size_t max_devices)
{
    if (devices == NULL || max_devices == 0) {
        return 0;
    }

    xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    size_t count = s_device_count < max_devices ? s_device_count : max_devices;
    memcpy(devices, s_devices, count * sizeof(*devices));
    xSemaphoreGive(s_devices_mutex);
    return count;
}

uint32_t ble_scanner_get_scan_id(void)
{
    return s_scan_id;
}

const char *ble_scanner_address_type_name(uint8_t address_type)
{
    switch (address_type) {
    case BLE_ADDR_PUBLIC:
        return "PUBLIC";
    case BLE_ADDR_RANDOM:
        return "RANDOM";
    case BLE_ADDR_PUBLIC_ID:
        return "PUBLIC ID";
    case BLE_ADDR_RANDOM_ID:
        return "RANDOM ID";
    default:
        return "UNKNOWN";
    }
}
