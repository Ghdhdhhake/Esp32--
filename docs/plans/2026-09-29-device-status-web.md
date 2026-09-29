# ESP32 Device Status Web Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Turn the ESP32 into a protected Wi-Fi access point that serves a responsive device-status dashboard at `http://192.168.4.1`.

**Architecture:** Run Wi-Fi in AP+STA mode so the board can host a local network and scan nearby access points. Serve one embedded HTML/CSS/JavaScript page with ESP-IDF's HTTP server; expose JSON endpoints for live status and an on-demand Wi-Fi rescan. Keep all assets in flash and require no internet connection.

**Tech Stack:** ESP-IDF 5.5.5, ESP Wi-Fi, ESP HTTP Server, cJSON, embedded HTML/CSS/JavaScript.

---

### Task 1: Add the embedded dashboard

**Files:**
- Create: `main/web/index.html`

**Step 1:** Create the complete dashboard as a standalone HTML file that CMake embeds into the firmware.

**Step 2:** Add accessible status cards for chip, flash, free heap, uptime, and connected clients.

**Step 3:** Add a network table, loading/error states, and a rescan button.

**Step 4:** Fetch `/api/status` on load and every two seconds; fetch `/api/scan` only when requested.

**Step 5:** Verify that the page has no external URLs, libraries, fonts, or assets.

### Task 2: Add AP, scan, and HTTP firmware

**Files:**
- Modify: `main/main.c`

**Step 1:** Preserve robust NVS initialization.

**Step 2:** Initialize default AP and STA network interfaces, configure AP+STA mode, and start the password-protected `ESP32-Manager` network.

**Step 3:** Retain up to 20 scan records in memory and make rescans replace the old snapshot only after a successful scan.

**Step 4:** Implement `/` to return `WEB_PAGE`.

**Step 5:** Implement `/api/status` with cJSON, including chip model, cores, revision, flash, heap, uptime, clients, and scan records.

**Step 6:** Implement `/api/scan` to refresh scan records and return the same status JSON.

**Step 7:** Log the SSID, password, and URL after the server starts.

### Task 3: Declare dependencies and verify

**Files:**
- Modify: `main/CMakeLists.txt`

**Step 1:** Add `esp_http_server`, `esp_timer`, `json`, `spi_flash`, and `esp_system` dependencies and embed `web/index.html` as text.

**Step 2:** Build with the installed ESP-IDF 5.5.5 toolchain.

Run: `idf.py build`

Expected: `sample_project.bin` is generated with no compiler or linker errors and fits the application partition.

**Step 3:** Flash and monitor from the ESP-IDF terminal.

Run: `idf.py -p COM8 flash monitor`

Expected: the log prints `ESP32-Manager`, `esp32admin`, and `http://192.168.4.1`.

**Step 4:** Connect a phone to the AP and verify the dashboard loads, values refresh, and Rescan updates the Wi-Fi table.
