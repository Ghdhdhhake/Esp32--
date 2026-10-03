#pragma once

#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/**
 * The slice of the dashboard that the chat tools are allowed to touch.
 *
 * main.c keeps its Wi-Fi scan buffers and the status serializer private; the
 * chat module goes through these three wrappers instead of reaching into them,
 * so there is exactly one place where "what an AI may do to this board" is
 * defined.
 */

/** Run a blocking Wi-Fi scan and publish the result for the dashboard. */
esp_err_t device_wifi_scan(void);

/** Array of the `limit` strongest networks, strongest first. */
cJSON *device_network_summary(size_t limit);

/** Full /api/status payload (device, ap, sta, led, networks). */
cJSON *device_status_snapshot(void);
