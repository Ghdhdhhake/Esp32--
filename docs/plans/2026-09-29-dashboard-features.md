# 单板仪表盘功能扩展 + BLE 扫描问题修复

**目标：** 在只有一个 ESP32 开发板的前提下，把网页控制台从"只能看状态"扩展成"能真正操作板子"的实验室工具；同时定位并修掉"蓝牙扫不到设备"的问题。

---

## 一、BLE 扫描扫不到设备：根因分析

### 1.1 先说结论

原实现里有 **一个真正的设计缺陷** 和 **两个环境级隐患**，三者都会表现为"点扫描没反应 / 扫不到设备"：

| # | 问题 | 严重度 | 说明 |
|---|---|---|---|
| A | 扫描是**阻塞式**的，HTTP 任务被占死 5–6.5 秒 | 高 | 浏览器请求超时/被丢弃，用户看到的是"没有任何设备"，而不是真正的错误码 |
| B | Wi-Fi 省电模式未关闭（默认 `WIFI_PS_MIN_MODEM`） | 高 | 单射频共存时 Wi-Fi 把电台时段占住，BLE 控制器拿不到扫描窗口 |
| C | 扫描占空比与失败原因完全不可见 | 中 | 没有任何诊断信息，任何失败都长得一样 |
| D | `ble_hs_util_ensure_addr()` 失败会导致 `ESP_ERROR_CHECK` 直接 panic | 中 | 失败即重启，连网页都进不去 |
| E | `s_ready` / `s_scanning` 跨任务无同步 | 低 | 竞态 |

### 1.2 为什么阻塞式扫描是致命的

原代码：

```c
static esp_err_t ble_scan_handler(httpd_req_t *request)
{
    esp_err_t err = ble_scanner_scan(5000);   /* 内部 xSemaphoreTake(..., 6500ms) */
    ...
}
```

`ble_scanner_scan()` 里 `xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(duration + 1500))`，
也就是 **HTTP 服务任务被阻塞 5–6.5 秒**。ESP-IDF 的 httpd 默认单任务处理所有请求，
于是：

- 浏览器这一次 `fetch('/api/ble/scan')` 挂 6.5 秒；
- 期间页面上每 2 秒一次的 `/api/status` 轮询全部排队/超时；
- 任何一个环节（浏览器、代理、TCP 重传）先放弃，前端就落到 `catch`，
  显示"蓝牙扫描失败"或者干脆显示空列表 —— **看起来就像"扫不到设备"**，
  但实际上扫描可能根本没跑完。

**修复：** 改成非阻塞状态机。

```
POST /api/ble/scan/start   -> 立刻返回 200，扫描在后台跑
GET  /api/ble/status       -> 前端每 700ms 轮询，实时拿进度 + 结果
POST /api/ble/scan/stop    -> 取消
```

### 1.3 为什么 Wi-Fi 省电会"吃掉"BLE

ESP32（经典款）只有一个 2.4 GHz 射频，Wi-Fi 和 BLE 靠 `esp_coex` 分时复用。
`esp_wifi_set_ps()` 的默认值是 `WIFI_PS_MIN_MODEM`：STA 会按自己的 DTIM 窗口
把射频"停"在自己的时序里。这个时序和 BLE 的扫描窗口（默认 10 ms 粒度）完全对不上，
共存仲裁器就会持续把 BLE 的扫描请求往后压。

**修复：** 启动 Wi-Fi 后显式调用

```c
esp_wifi_set_ps(WIFI_PS_NONE);
```

代价是空闲电流略高，收益是 BLE 扫描变可靠 —— 对一个插着 USB 的调试板来说完全划算。

### 1.4 扫描参数的选择

```c
.itvl   = 0x0060,   /* 60 ms 扫描间隔 */
.window = 0x0030,   /* 30 ms 扫描窗口 → 50% 占空比 */
.passive = 0,       /* 主动扫描：额外发 SCAN_REQ，能拿到更多名字 */
.filter_duplicates = 0,
.disable_observer_mode = 0,   /* 必须为 0，否则主机不上报广播报文 */
```

- 原代码把 `itvl` / `window` 留成 0，NimBLE 会用默认值 10 ms / 10 ms，
  即 **100% 占空比**。100% 占空比会把 Wi-Fi 饿死，网页本身就会卡。
- 50% 占空比下，广播设备（每 20–100 ms 在 3 个信道各发一次）在 1–2 秒内就能被听到，
  同时 Wi-Fi 还有一半时间可用。
- `.disable_observer_mode` 是 `struct ble_gap_disc_params` 里一个 `:1` 位域，
  漏填虽然默认是 0（正确），但显式写出来可以避免以后被人改坏。

### 1.5 新增的诊断能力

`GET /api/ble/status` 的 `diag` 字段把所有"为什么失败"都摊开：

| 字段 | 含义 |
|---|---|
| `host_ready` | NimBLE 主机是否完成同步 |
| `controller_status` / `controller_ok` | `esp_bt_controller_get_status()`，ENABLED=2 |
| `last_sync_rc` | `ble_hs_util_ensure_addr()` 返回码 |
| `last_scan_rc` | `ble_gap_disc()` 返回码（30=`BLE_HS_EDISABLED`，13=`BLE_HS_ETIMEOUT`） |
| `last_adv_rc` | `ble_gap_adv_start()` 返回码 |
| `adv_reports` / `adv_reports_scan` | **收到的原始广播报文数**（不是去重后的设备数） |
| `host_resets` | NimBLE 主机复位次数 |
| `scan_runs` / `scan_timeouts` | 扫描启动次数 / 完成事件丢失次数 |
| `own_address` | 本机 BLE 地址 |

关键判据：

- `adv_reports_scan == 0` 且 `last_scan_rc == 0` → **射频/共存问题**，或附近真的没有广播设备；
- `adv_reports_scan > 0` 但设备列表为空 → 去重/解析问题；
- `last_scan_rc == 30` → BLE 主机没起来；
- `last_scan_rc == 13` → 控制器没回 `DISC_COMPLETE`，HCI 卡死。

### 1.6 单板自检：让板子自己广播

只有一块板子时，最有效的排查方法是让板子 **自己发广播**，然后用手机上的
nRF Connect / 蓝牙调试助手去搜：

- 手机能搜到 `ESP32-DASH` → BLE **发射**通路正常；
- 网页扫描能看到手机/耳机 → BLE **接收**通路正常；
- 两条都通 → 问题不在硬件，回到上面的诊断计数器继续查。

因此本次把 NimBLE 的 **broadcaster** 角色也打开了（`CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y`），
并加了 `/api/ble/adv` 开关和自定义名称（存 NVS）。

### 1.7 其他健壮性修复

- `ble_scanner_init()` 超时/失败不再 `ESP_ERROR_CHECK` 崩溃，而是记录错误码后继续启动，
  保证网页能打开、能读到失败原因。
- `host_sync_callback()` 无论 `ensure_addr` 成败都释放信号量，避免启动挂死。
- 扫描完成事件丢失时，`refresh_scan_state()` 会在 `duration + 2.5s` 后把状态置为
  `error` 并计入 `scan_timeouts`，不会永久卡在 `running`。
- 设备列表按 RSSI 降序排列，并统计每个地址的出现次数。

---

## 二、新增的网页功能

### 2.1 板载 LED 控制（用户点名）

- 驱动方式：**LEDC PWM**（10 bit / 5 kHz），不是简单的 `gpio_set_level`，
  所以亮度、闪烁、呼吸都能做。
- 四种模式：关闭 / 常亮 / 闪烁 / 呼吸（三角波，10 ms 刷新）。
- 亮度 0–100%，周期 100–5000 ms。
- **引脚可配**（默认 GPIO2），**极性可切**（高电平点亮 / 低电平点亮）——
  因为不同开发板的板载 LED 接法不一样，EsprEssif 官方 DevKitC 是低电平点亮，
  大多数国产克隆板是高电平点亮。
- 设置写入 NVS，重启后保留。
- 引脚合法性校验：GPIO6–11（SPI Flash）、GPIO34–39（仅输入）直接拒绝并给出提示。

### 2.2 引脚工具箱（GPIO + ADC1）

- 任意合法 GPIO 输出高/低、释放为高阻、读取电平（可选内部上拉）。
- ADC1 八个通道（GPIO36/37/38/39/32/33/34/35）电压测量，
  使用 `adc_cali` 线性拟合校准（ESP32 经典款只支持 line fitting，不支持 curve fitting），
  网页上画实时折线。
- 只开放 ADC1：ADC2 与 Wi-Fi 冲突，在跑 AP 的板子上根本不能用。

### 2.3 STA 联网

- 扫描结果直接作为 SSID 下拉候选；
- 一键连接 / 断开，显示 IP、网关、RSSI。
- 提示：APSTA 模式下 AP 信道跟随 STA 信道，切换路由器后客户端会掉线一次。

### 2.4 设备信息

芯片型号/版本/核数、Flash 容量、可用堆/最小可用堆、IDF 版本、**复位原因**、
STA/AP/BT 三个 MAC、AP 信道、日志行数。

### 2.5 系统日志（RAM 环形缓冲）

- 通过 `esp_log_set_vprintf()` 钩住 ESP-IDF 日志，镜像到 64 行 × 128 字节的环形缓冲；
- `GET /api/logs` 读取，网页自动跟随滚动。
- **这是没有串口线时最重要的调试手段**：BLE 的失败原因会直接打印在网页上。

### 2.6 系统操作

- 重启、恢复出厂设置（清 NVS）。

### 2.7 Wi-Fi 扫描（保留并改进）

保留原有面板，表格新增"信号条"，SSID 同时喂给 STA 面板的候选列表。

---

## 三、构建配置变更

### 3.1 分区表

原 `partitions_singleapp.csv` 只给 factory 应用 1 MiB，而原固件已经 **982 KB**，
加上 LEDC/ADC/日志/更多路由必然溢出。改为自定义 `partitions.csv`：

```
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x180000,   # 1.5 MiB
```

板载 Flash 为 2 MiB，1.5 MiB 应用槽 + 0x10000 起始偏移 = 1.56 MiB，留有余量。

### 3.2 蓝牙角色

`CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y`（新增），observer 保留，central/peripheral 仍关闭。

### 3.3 HTTP 服务器

- `max_uri_handlers` 从默认 8 提到 **20**（现在注册 14 条路由，原配置会直接注册失败）；
- `stack_size` 10240（cJSON + 40 条 BLE 设备记录的局部数组）；
- `lru_purge_enable` 保留。

---

## 四、验证

- 5 个源文件全部通过 `xtensa-esp32-elf-gcc -fsyntax-only` 检查；
- `idf.py build` 完整编译通过，固件小于 1.5 MiB 分区；
- 未在真机验证（本机没有接开发板），需上板确认：
  1. 打开网页，LED 面板点"常亮"，观察板载 LED；
  2. BLE 面板开广播，手机搜 `ESP32-DASH`；
  3. BLE 面板扫描 8 秒，看设备表和诊断计数器。

### 4.1 在 Git Bash 里构建

`idf.py` 的入口是这样的：

```python
if __name__ == '__main__':
    if 'MSYSTEM' in os.environ:
        print_warning('MSys/Mingw is no longer supported...')
    else:
        main()          # <-- MSYS 分支里根本没有调用 main()
```

Git Bash 永远会导出 `MSYSTEM=MINGW64`，所以直接敲 `idf.py build`
**只会打印一行警告然后退出，什么都不编译**（退出码还是 0，非常容易误判）。

为此增加了 `tools/build.sh`：剥掉 `MSYSTEM`，直接 `import idf; idf.main()`。
另外它在编译前会把 xtensa 工具链 / cmake / ninja 加进 PATH。

```bash
tools/build.sh                 # 编译
tools/build.sh -p COM8 flash monitor
```

### 4.2 已知的非致命警告

CMake 会报一次 `Error while generating esp_rom gdbinit`（`ESP_ROM_ELF_DIR`
环境变量未定义）。这只影响 GDB 加载 ROM 符号，**不影响编译和烧写**。

