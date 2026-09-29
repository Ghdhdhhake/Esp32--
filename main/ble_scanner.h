#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * BLE radio services for the dashboard.
 *
 * Two independent roles are exposed:
 *
 *   - Observer  : scan for nearby advertisers (the "BLE spectrum" panel).
 *   - Broadcaster: advertise the board itself so a phone can find it. This is
 *                  also the cheapest way to prove the BLE radio actually
 *                  transmits when you only own a single board.
 *
 * Scanning is deliberately *non blocking*: starting a scan returns immediately
 * and the web UI polls /api/ble/status. The previous blocking design held the
 * HTTP task for 5-6.5 s, which made every failure look like "no devices found"
 * because the browser request simply died.
 */

#define BLE_SCANNER_MAX_DEVICES 40
#define BLE_SCANNER_NAME_SIZE 32
#define BLE_SCANNER_ADV_NAME_SIZE 24
#define BLE_SCANNER_DEFAULT_SCAN_MS 8000
#define BLE_SCANNER_MAX_SCAN_MS 30000

typedef struct {
    char name[BLE_SCANNER_NAME_SIZE];
    char address[18];
    int8_t rssi;
    uint8_t address_type;
    uint16_t seen_count;
} ble_scanner_device_t;

typedef enum {
    BLE_SCAN_IDLE = 0,
    BLE_SCAN_RUNNING,
    BLE_SCAN_DONE,
    BLE_SCAN_ERROR,
} ble_scan_state_t;

/** Everything needed to explain *why* a scan returned nothing. */
typedef struct {
    bool host_ready;        /* NimBLE host finished syncing                */
    bool controller_ok;     /* ESP BT controller reports ENABLED           */
    int controller_status;  /* esp_bt_controller_status_t                  */
    int last_sync_rc;       /* rc of ble_hs_util_ensure_addr()             */
    int last_scan_rc;       /* rc of ble_gap_disc()                        */
    uint32_t host_resets;   /* NimBLE host reset callbacks                 */
    ble_scan_state_t state;
    uint32_t scan_duration_ms;
    uint32_t scan_elapsed_ms;
    uint32_t adv_reports;       /* raw BLE_GAP_EVENT_DISC events since boot */
    uint32_t adv_reports_scan;  /* raw reports during the current/last scan */
    uint32_t scan_runs;
    uint32_t scan_timeouts;
    uint32_t unique_devices;
    bool advertising;
    int last_adv_rc; /* rc of the last ble_gap_adv_start() attempt */
    char adv_name[BLE_SCANNER_ADV_NAME_SIZE];
    uint8_t own_address[6];
    uint8_t own_address_type;
    bool own_address_valid;
    uint32_t adv_starts;
} ble_scanner_diag_t;

esp_err_t ble_scanner_init(void);

/** Kick off a scan and return immediately. Safe to call while one is running. */
esp_err_t ble_scanner_start_scan(uint32_t duration_ms);

/** Cancel the running scan (no-op when idle). */
esp_err_t ble_scanner_stop_scan(void);

size_t ble_scanner_get_devices(ble_scanner_device_t *devices,
                               size_t max_devices);

uint32_t ble_scanner_get_scan_id(void);

const char *ble_scanner_address_type_name(uint8_t address_type);

void ble_scanner_get_diag(ble_scanner_diag_t *out);

/** Start advertising under `name` (NULL keeps the stored name). */
esp_err_t ble_scanner_advertising_start(const char *name);

esp_err_t ble_scanner_advertising_stop(void);

/** Restore the persisted advertising preference from NVS. */
esp_err_t ble_scanner_load(void);
