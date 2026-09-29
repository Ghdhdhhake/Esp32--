#include "log_ring.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define LOG_RING_SLOTS 64
#define LOG_RING_LINE 128

typedef struct {
    uint32_t seq;
    uint32_t milliseconds;
    char text[LOG_RING_LINE];
} log_ring_entry_t;

static log_ring_entry_t s_slots[LOG_RING_SLOTS];
static volatile uint32_t s_seq;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_previous_vprintf;
static bool s_installed;

static void log_ring_push(const char *text)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t seq = s_seq + 1;
    s_seq = seq;
    log_ring_entry_t *slot = &s_slots[(seq - 1) % LOG_RING_SLOTS];
    slot->seq = seq;
    slot->milliseconds = (uint32_t)(esp_timer_get_time() / 1000);
    strlcpy(slot->text, text, sizeof(slot->text));
    portEXIT_CRITICAL(&s_lock);
}

static int log_ring_vprintf(const char *format, va_list arguments)
{
    char buffer[LOG_RING_LINE];
    va_list copy;
    va_copy(copy, arguments);
    int length = vsnprintf(buffer, sizeof(buffer), format, copy);
    va_end(copy);

    if (length > 0) {
        size_t text_length = (size_t)length;
        if (text_length >= sizeof(buffer)) {
            text_length = sizeof(buffer) - 1;
        }
        while (text_length > 0 &&
               (buffer[text_length - 1] == '\n' ||
                buffer[text_length - 1] == '\r')) {
            buffer[--text_length] = '\0';
        }
        log_ring_push(buffer);
    }

    if (s_previous_vprintf != NULL) {
        return s_previous_vprintf(format, arguments);
    }
    return vprintf(format, arguments);
}

void log_ring_init(void)
{
    if (s_installed) {
        return;
    }
    s_previous_vprintf = esp_log_set_vprintf(log_ring_vprintf);
    s_installed = true;
}

size_t log_ring_snapshot(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    uint32_t newest = s_seq;
    uint32_t count = newest < LOG_RING_SLOTS ? newest : LOG_RING_SLOTS;
    uint32_t first = newest - count;
    size_t written = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t expected = first + i + 1;
        uint32_t index = (expected - 1) % LOG_RING_SLOTS;
        log_ring_entry_t local;

        portENTER_CRITICAL(&s_lock);
        local = s_slots[index];
        portEXIT_CRITICAL(&s_lock);

        if (local.seq != expected) {
            /* The slot was recycled while we were copying; skip it. */
            continue;
        }

        size_t remaining = out_size - written;
        if (remaining < 2) {
            break;
        }
        int result = snprintf(out + written, remaining, "%8u  %s\n",
                              (unsigned)local.milliseconds, local.text);
        if (result < 0 || (size_t)result >= remaining) {
            break;
        }
        written += (size_t)result;
    }

    return written;
}

uint32_t log_ring_total_lines(void)
{
    return s_seq;
}
