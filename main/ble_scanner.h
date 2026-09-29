#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BLE_SCANNER_MAX_DEVICES 30
#define BLE_SCANNER_NAME_SIZE 32

typedef struct {
    char name[BLE_SCANNER_NAME_SIZE];
    char address[18];
    int8_t rssi;
    uint8_t address_type;
} ble_scanner_device_t;

esp_err_t ble_scanner_init(void);
esp_err_t ble_scanner_scan(uint32_t duration_ms);
size_t ble_scanner_get_devices(ble_scanner_device_t *devices,
                               size_t max_devices);
uint32_t ble_scanner_get_scan_id(void);
const char *ble_scanner_address_type_name(uint8_t address_type);
