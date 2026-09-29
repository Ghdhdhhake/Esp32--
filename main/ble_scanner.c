#include "ble_scanner.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"

static const char *TAG = "ble_scanner";

/* Scan timing, expressed in 0.625 ms units as the HCI layer expects.
 * 60 ms interval / 30 ms window = 50 % duty cycle. That is the sweet spot when
 * Wi-Fi and BLE share the single 2.4 GHz radio: BLE still hears every
 * advertiser within a second or two, and the Wi-Fi AP keeps serving the
 * dashboard. A 100 % duty scan starves Wi-Fi and can make the page time out. */
#define BLE_SCANNER_SCAN_ITVL 0x0060
#define BLE_SCANNER_SCAN_WINDOW 0x0030

/* Grace period after the requested duration before declaring a scan lost. */
#define BLE_SCANNER_SCAN_GRACE_MS 2500

#define BLE_NVS_NAMESPACE "blecfg"
#define BLE_DEFAULT_ADV_NAME "ESP32-DASH"

static ble_scanner_device_t s_devices[BLE_SCANNER_MAX_DEVICES];
static size_t s_device_count;
static uint32_t s_scan_id;
static bool s_ready;
static bool s_scanning;
static bool s_initialized;
static ble_scan_state_t s_state = BLE_SCAN_IDLE;
static uint32_t s_scan_duration_ms;
static int64_t s_scan_start_us;

/* s_state / s_scanning / s_scan_id are touched by two different tasks:
 *   - the NimBLE host task, via BLE_GAP_EVENT_DISC_COMPLETE -> finish_scan()
 *   - the HTTP task, via the timeout fallback and the start/stop endpoints
 * The spinlock keeps every transition atomic and, together with the
 * `was_running` guard in finish_scan(), makes a scan close exactly once even
 * when the controller event and the timeout fallback race each other. */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t s_devices_mutex;
static SemaphoreHandle_t s_sync_done;

/* Diagnostics ------------------------------------------------------------- */
static volatile uint32_t s_adv_reports;
static volatile uint32_t s_adv_reports_scan;
static volatile uint32_t s_scan_runs;
static volatile uint32_t s_scan_timeouts;
static volatile uint32_t s_host_resets;
static volatile uint32_t s_adv_starts;
static volatile int s_last_sync_rc;
static volatile int s_last_scan_rc;
static volatile int s_last_adv_rc;
static volatile bool s_advertising;
static uint8_t s_own_address[6];
static uint8_t s_own_address_type;
static bool s_own_address_valid;
static char s_adv_name[BLE_SCANNER_ADV_NAME_SIZE] = BLE_DEFAULT_ADV_NAME;

/* Helpers ----------------------------------------------------------------- */

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

static void format_own_address(char *destination, size_t destination_size)
{
    snprintf(destination, destination_size,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             s_own_address[5], s_own_address[4], s_own_address[3],
             s_own_address[2], s_own_address[1], s_own_address[0]);
}

static void sort_devices_by_rssi(void)
{
    for (size_t i = 1; i < s_device_count; i++) {
        ble_scanner_device_t key = s_devices[i];
        size_t j = i;
        while (j > 0 && s_devices[j - 1].rssi < key.rssi) {
            s_devices[j] = s_devices[j - 1];
            j--;
        }
        s_devices[j] = key;
    }
}

static void save_advertisement(const struct ble_gap_disc_desc *discovery)
{
    if (s_devices_mutex == NULL) {
        return;
    }

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
    if (s_devices[index].seen_count < UINT16_MAX) {
        s_devices[index].seen_count++;
    }
    if (fields_valid && fields.name != NULL && fields.name_len > 0) {
        copy_safe_name(s_devices[index].name, sizeof(s_devices[index].name),
                       fields.name, fields.name_len);
    }

    xSemaphoreGive(s_devices_mutex);
}

/** Close the running scan.
 *
 * Returns true when this call is the one that actually closed it. That lets the
 * timeout fallback know whether its diagnosis still applies, and stops a late
 * controller event from overwriting the return code of whichever path got
 * there first. */
static bool finish_scan(bool ok, int reason)
{
    portENTER_CRITICAL(&s_state_lock);
    bool was_running = s_scanning;
    if (was_running) {
        s_scanning = false;
        s_state = ok ? BLE_SCAN_DONE : BLE_SCAN_ERROR;
        s_scan_id++;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!was_running) {
        /* The other path already closed this scan - the controller event and
         * the timeout fallback must not both be counted. */
        return false;
    }

    s_last_scan_rc = reason;

    size_t unique = 0;
    if (s_devices_mutex != NULL) {
        xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
        unique = s_device_count;
        xSemaphoreGive(s_devices_mutex);
    }

    ESP_LOGI(TAG, "BLE scan finished: %" PRIu32 " raw report(s), %u unique "
                  "device(s), rc=%d",
             s_adv_reports_scan, (unsigned)unique, reason);
    return true;
}

/** Detect a scan whose completion event never arrived (controller hiccup). */
static void refresh_scan_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool running = (s_state == BLE_SCAN_RUNNING);
    portEXIT_CRITICAL(&s_state_lock);

    if (!running) {
        return;
    }

    int64_t elapsed_us = esp_timer_get_time() - s_scan_start_us;
    uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000);
    if (elapsed_ms <= s_scan_duration_ms + BLE_SCANNER_SCAN_GRACE_MS) {
        return;
    }

    /* Only blame the timeout if we are the ones closing the scan. */
    if (!finish_scan(false, BLE_HS_ETIMEOUT)) {
        return;
    }

    s_scan_timeouts++;
    ESP_LOGW(TAG, "BLE scan completion event never arrived (%" PRIu32
                  " ms elapsed) - controller/HCI stall",
             elapsed_ms);
}

static int gap_event_handler(struct ble_gap_event *event, void *argument)
{
    (void)argument;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        s_adv_reports++;
        s_adv_reports_scan++;
        save_advertisement(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        finish_scan(true, event->disc_complete.reason);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        ESP_LOGI(TAG, "BLE advertising stopped (reason=%d)",
                 event->adv_complete.reason);
        return 0;

    default:
        return 0;
    }
}

/* Host lifecycle ---------------------------------------------------------- */

static void host_reset_callback(int reason)
{
    s_ready = false;
    s_scanning = false;
    s_advertising = false;
    s_state = BLE_SCAN_ERROR;
    s_host_resets++;
    ESP_LOGE(TAG, "NimBLE host reset; reason=%d (total %" PRIu32 ")",
             reason, s_host_resets);
}

static void host_sync_callback(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    s_last_sync_rc = rc;
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed rc=%d", rc);
        /* Fall through: still release the init waiter so the dashboard can
         * boot and report the problem instead of hanging in a boot loop. */
        xSemaphoreGive(s_sync_done);
        return;
    }

    uint8_t address_type = 0;
    rc = ble_hs_id_infer_auto(0, &address_type);
    if (rc == 0) {
        uint8_t address[6] = {0};
        if (ble_hs_id_copy_addr(address_type, address, NULL) == 0) {
            memcpy(s_own_address, address, sizeof(s_own_address));
            s_own_address_type = address_type;
            s_own_address_valid = true;
            char text[18];
            format_own_address(text, sizeof(text));
            ESP_LOGI(TAG, "BLE identity address %s (type %s)", text,
                     ble_scanner_address_type_name(address_type));
        }
    }

    s_ready = true;
    xSemaphoreGive(s_sync_done);
    ESP_LOGI(TAG, "NimBLE observer/broadcaster ready");
}

static void host_task(void *parameter)
{
    (void)parameter;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Advertising ------------------------------------------------------------- */

static esp_err_t advertising_save(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BLE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, "advname", s_adv_name);
    if (err == ESP_OK) {
        uint8_t enabled = s_advertising ? 1 : 0;
        err = nvs_set_u8(handle, "adven", enabled);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t advertising_start_locked(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_advertising) {
        return ESP_OK;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)s_adv_name;
    fields.name_len = (uint8_t)strlen(s_adv_name);
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        s_last_adv_rc = rc;
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed rc=%d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_NON;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;

    uint8_t own_address_type = 0;
    if (ble_hs_id_infer_auto(0, &own_address_type) != 0) {
        own_address_type = 0;
    }

    rc = ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER,
                           &parameters, gap_event_handler, NULL);
    s_last_adv_rc = rc;
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed rc=%d", rc);
        return ESP_FAIL;
    }

    s_advertising = true;
    s_adv_starts++;
    ESP_LOGI(TAG, "BLE advertising as \"%s\"", s_adv_name);
    return ESP_OK;
}

esp_err_t ble_scanner_advertising_start(const char *name)
{
    if (name != NULL && name[0] != '\0') {
        strlcpy(s_adv_name, name, sizeof(s_adv_name));
    }
    esp_err_t err = advertising_start_locked();
    if (err == ESP_OK) {
        advertising_save();
    }
    return err;
}

esp_err_t ble_scanner_advertising_stop(void)
{
    if (!s_advertising) {
        advertising_save();
        return ESP_OK;
    }

    int rc = ble_gap_adv_stop();
    s_advertising = false;
    advertising_save();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_adv_stop rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "BLE advertising stopped");
    return ESP_OK;
}

esp_err_t ble_scanner_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BLE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t length = sizeof(s_adv_name);
    if (nvs_get_str(handle, "advname", s_adv_name, &length) != ESP_OK ||
        s_adv_name[0] == '\0') {
        strlcpy(s_adv_name, BLE_DEFAULT_ADV_NAME, sizeof(s_adv_name));
    }

    uint8_t enabled = 0;
    bool want_advertising = nvs_get_u8(handle, "adven", &enabled) == ESP_OK &&
                            enabled != 0;
    nvs_close(handle);

    if (want_advertising) {
        esp_err_t start_err = advertising_start_locked();
        if (start_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not restore advertising: %s",
                     esp_err_to_name(start_err));
        }
    }
    return ESP_OK;
}

/* Public API -------------------------------------------------------------- */

esp_err_t ble_scanner_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_devices_mutex = xSemaphoreCreateMutex();
    s_sync_done = xSemaphoreCreateBinary();
    if (s_devices_mutex == NULL || s_sync_done == NULL) {
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

    if (xSemaphoreTake(s_sync_done, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for the NimBLE host to sync");
        return ESP_ERR_TIMEOUT;
    }

    if (!s_ready) {
        ESP_LOGE(TAG, "NimBLE host synced but the identity address is unusable "
                      "(rc=%d)", s_last_sync_rc);
        s_initialized = true; /* keep the dashboard alive for diagnostics */
        return ESP_FAIL;
    }

    s_initialized = true;
    ble_scanner_load();
    return ESP_OK;
}

esp_err_t ble_scanner_start_scan(uint32_t duration_ms)
{
    refresh_scan_state();

    if (!s_ready) {
        s_last_scan_rc = BLE_HS_EDISABLED;
        portENTER_CRITICAL(&s_state_lock);
        s_state = BLE_SCAN_ERROR;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGW(TAG, "Scan refused: BLE host is not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_devices_mutex == NULL) {
        ESP_LOGE(TAG, "Scan refused: the scanner was never initialised");
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_state_lock);
    bool already_running = s_scanning;
    portEXIT_CRITICAL(&s_state_lock);
    if (already_running) {
        ESP_LOGW(TAG, "Scan refused: a scan is already running");
        return ESP_ERR_INVALID_STATE;
    }

    if (duration_ms < 1000) {
        duration_ms = 1000;
    }
    if (duration_ms > BLE_SCANNER_MAX_SCAN_MS) {
        duration_ms = BLE_SCANNER_MAX_SCAN_MS;
    }

    xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    xSemaphoreGive(s_devices_mutex);
    s_adv_reports_scan = 0;

    uint8_t own_address_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_address_type);
    if (rc != 0) {
        s_last_scan_rc = rc;
        portENTER_CRITICAL(&s_state_lock);
        s_state = BLE_SCAN_ERROR;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
        return ESP_FAIL;
    }
    s_own_address_type = own_address_type;

    struct ble_gap_disc_params parameters = {
        .itvl = BLE_SCANNER_SCAN_ITVL,
        .window = BLE_SCANNER_SCAN_WINDOW,
        .filter_duplicates = 0,
        .passive = 0,
        .filter_policy = 0,
        .limited = 0,
        .disable_observer_mode = 0,
    };

    /* Publish the timing *before* flagging the scan as running:
     * refresh_scan_state() measures esp_timer_get_time() against
     * s_scan_start_us and would otherwise compare against the previous scan's
     * timestamp and declare an instant timeout. */
    s_scan_duration_ms = duration_ms;
    s_scan_start_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_state_lock);
    s_scanning = true;
    s_state = BLE_SCAN_RUNNING;
    portEXIT_CRITICAL(&s_state_lock);

    rc = ble_gap_disc(own_address_type, (int32_t)duration_ms, &parameters,
                      gap_event_handler, NULL);
    s_last_scan_rc = rc;
    if (rc != 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_scanning = false;
        s_state = BLE_SCAN_ERROR;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGE(TAG, "ble_gap_disc failed rc=%d (itvl=0x%04X window=0x%04X)",
                 rc, BLE_SCANNER_SCAN_ITVL, BLE_SCANNER_SCAN_WINDOW);
        return ESP_FAIL;
    }

    s_scan_runs++;
    ESP_LOGI(TAG, "BLE scan started (%" PRIu32 " ms, active scan, 50%% duty)",
             duration_ms);
    return ESP_OK;
}

esp_err_t ble_scanner_stop_scan(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool running = s_scanning;
    portEXIT_CRITICAL(&s_state_lock);

    if (!running) {
        return ESP_OK;
    }

    /* ble_gap_disc_cancel() reports back as BLE_GAP_EVENT_DISC_COMPLETE, which
     * is what clears s_scanning via finish_scan(). */
    int rc = ble_gap_disc_cancel();
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc_cancel rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

size_t ble_scanner_get_devices(ble_scanner_device_t *devices,
                               size_t max_devices)
{
    if (devices == NULL || max_devices == 0 || s_devices_mutex == NULL) {
        return 0;
    }

    refresh_scan_state();

    xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    sort_devices_by_rssi();
    size_t count = s_device_count < max_devices ? s_device_count : max_devices;
    memcpy(devices, s_devices, count * sizeof(*devices));
    xSemaphoreGive(s_devices_mutex);
    return count;
}

uint32_t ble_scanner_get_scan_id(void)
{
    return s_scan_id;
}

void ble_scanner_get_diag(ble_scanner_diag_t *out)
{
    if (out == NULL) {
        return;
    }

    refresh_scan_state();

    memset(out, 0, sizeof(*out));
    out->host_ready = s_ready;
    out->controller_status = (int)esp_bt_controller_get_status();
    out->controller_ok =
        out->controller_status == ESP_BT_CONTROLLER_STATUS_ENABLED;
    out->last_sync_rc = s_last_sync_rc;
    out->last_scan_rc = s_last_scan_rc;
    out->host_resets = s_host_resets;
    out->state = s_state;
    out->scan_duration_ms = s_scan_duration_ms;
    out->adv_reports = s_adv_reports;
    out->adv_reports_scan = s_adv_reports_scan;
    out->scan_runs = s_scan_runs;
    out->scan_timeouts = s_scan_timeouts;
    out->advertising = s_advertising;
    out->last_adv_rc = s_last_adv_rc;
    out->adv_starts = s_adv_starts;
    out->own_address_type = s_own_address_type;
    out->own_address_valid = s_own_address_valid;
    memcpy(out->own_address, s_own_address, sizeof(out->own_address));
    strlcpy(out->adv_name, s_adv_name, sizeof(out->adv_name));

    if (s_devices_mutex != NULL) {
        xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
        out->unique_devices = (uint32_t)s_device_count;
        xSemaphoreGive(s_devices_mutex);
    }

    if (s_state == BLE_SCAN_RUNNING) {
        uint32_t elapsed = (uint32_t)((esp_timer_get_time() - s_scan_start_us) /
                                      1000);
        out->scan_elapsed_ms = elapsed;
    } else {
        out->scan_elapsed_ms = s_scan_duration_ms;
    }
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
