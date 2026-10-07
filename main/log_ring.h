#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * 固件运行日志的内存镜像接口。日志总量受固定槽位限制，读取端获得的是
 * 最近的稳定快照，适合直接返回给网页而不会占用大量堆内存。
 *
 * In-RAM log capture.
 *
 * ESP-IDF's log output is mirrored into a small ring buffer so the web UI can
 * show what the firmware is doing without a serial cable attached. This is the
 * single most useful debugging tool when something like the BLE scanner
 * misbehaves: the reason shows up on the page instead of only on UART0.
 */

void log_ring_init(void);

/**
 * 将最近日志行复制到 `out`，各行以换行符分隔。
 *
 * @return 已写入字节数（不含结尾 NUL）。
 */
size_t log_ring_snapshot(char *out, size_t out_size);

/** 返回自启动以来已捕获的日志总行数。 */
uint32_t log_ring_total_lines(void);
