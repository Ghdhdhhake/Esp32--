#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * In-RAM log capture.
 *
 * ESP-IDF's log output is mirrored into a small ring buffer so the web UI can
 * show what the firmware is doing without a serial cable attached. This is the
 * single most useful debugging tool when something like the BLE scanner
 * misbehaves: the reason shows up on the page instead of only on UART0.
 */

void log_ring_init(void);

/**
 * Copy the most recent log lines into `out` as one newline separated string.
 *
 * @return number of bytes written (excluding the terminating NUL).
 */
size_t log_ring_snapshot(char *out, size_t out_size);

/** Total number of lines captured since boot. */
uint32_t log_ring_total_lines(void);
