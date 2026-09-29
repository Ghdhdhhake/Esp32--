# BLE Scanner Web Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add an on-demand BLE scan to the ESP32 device dashboard and display nearby advertisers with name, address, RSSI, and address type.

**Architecture:** Enable ESP-IDF's BLE-only NimBLE host and observer role alongside the existing Wi-Fi AP+STA stack. A NimBLE host task collects and deduplicates advertisement reports during a five-second scan; the HTTP endpoint waits for completion and returns a JSON snapshot. The existing embedded page adds a second instrument panel without external assets.

**Tech Stack:** ESP-IDF 5.5.5, NimBLE observer, FreeRTOS semaphores, ESP HTTP Server, cJSON, embedded HTML/CSS/JavaScript.

---

### Task 1: Enable BLE-only NimBLE

**Files:**
- Modify: `sdkconfig`
- Create: `sdkconfig.defaults`

**Step 1:** Enable `CONFIG_BT_ENABLED`, `CONFIG_BT_NIMBLE_ENABLED`, the ESP controller, BLE-only controller mode, and observer role.

**Step 2:** Disable Bluedroid and unneeded NimBLE central, peripheral, and broadcaster roles to reduce memory usage.

**Step 3:** Reconfigure the project and confirm the generated configuration retains the requested symbols.

### Task 2: Implement the BLE scanner and API

**Files:**
- Create: `main/ble_scanner.h`
- Create: `main/ble_scanner.c`
- Modify: `main/main.c`
- Modify: `main/CMakeLists.txt`

**Step 1:** Initialize NimBLE, wait for host synchronization, and run its FreeRTOS host task.

**Step 2:** Start a five-second active scan on demand and block the caller on a completion semaphore.

**Step 3:** Parse advertising names safely, format addresses, deduplicate up to 30 devices, and retain the strongest/current RSSI.

**Step 4:** Add `/api/ble/scan`, returning `ble_devices`, `ble_count`, and `ble_scan_id` as JSON.

**Step 5:** Reject concurrent scans with an explicit busy error.

### Task 3: Extend the embedded dashboard

**Files:**
- Modify: `main/web/index.html`

**Step 1:** Add a visually distinct BLE spectrum panel consistent with the existing industrial telemetry theme.

**Step 2:** Add a Scan BLE button with a five-second progress state and error feedback.

**Step 3:** Render name, address, RSSI bars, and address type using DOM text nodes.

### Task 4: Build and size verification

**Files:**
- Verify: `build/sample_project.bin`

**Step 1:** Build with ESP-IDF 5.5.5.

Run: `idf.py build`

Expected: BLE and Wi-Fi coexistence code compiles and links without warnings promoted to errors.

**Step 2:** Confirm the application fits the configured partition; enlarge only the factory application partition if NimBLE exceeds the current 1 MiB slot.

**Step 3:** Flash, connect to `ESP32-Manager`, open `http://192.168.4.1`, and verify BLE Scan returns nearby advertisers while status polling resumes afterward.
