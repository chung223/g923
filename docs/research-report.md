# Logitech G923 TRUEFORCE (PS4/PS5/PC 版) 在 macOS 上的驅動設計與研究報告

- 版本：2026-09-08（synthesis of six verified research reports）
- 目標機器：macOS 26.2 (Tahoe)、Apple Silicon (arm64)、SIP **enabled**、Xcode 26.6 / Swift 6.3、DriverKit SDK、Homebrew sdl3 3.4.16 / sdl2-compat 2.32.72、G HUB 1.1.23 已安裝
- 硬體狀態：實體方向盤**尚未取得**，本文所有結論均來自原始碼、官方文件與逆向工程資料；每一項都標註 confidence 與來源
- 本文對象：orchestrator / 實作者。第 3 節的位元組表可直接照抄成程式碼

---

## 1. 結論摘要

### 1.1 一句話結論

在 SIP 開啟、不裝 kext 的 Apple Silicon Mac 上，**G923 PS 版的 force feedback 是可以做到的**，而且完全在 user space 完成：用 `IOHIDManager`/`IOHIDDevice` 開啟方向盤 USB interface 0，以 `IOHIDDeviceSetReport` 送出 Logitech「classic」7-byte 指令（與 G29 相同的 lg4ff protocol）。做不到的只有兩件事：(a) 透過 Apple 的 `ForceFeedback.framework` plug-in 機制讓「所有」原生遊戲自動獲得 FFB；(b) 遊戲端授權的 TrueForce 內容。

### 1.2 可行 / 不可行對照表

| 項目 | 結論 | Confidence | 關鍵證據 |
|---|---|---|---|
| 讀取輸入（方向盤角度、踏板、按鍵、H 排檔） | **可行**，macOS 已當 generic HID / GameController 裝置；PC mode (c266) 的 12-byte input report 與 G29 完全相同 | high | lgff_wheel_adapter `usb_descriptors.h:515-580`、LogiWheelHost（在真 G923 c266 上驗證按鍵 mask） |
| PS mode (c267) → PC/Classic mode (c266) 切換 | **可行**，一個 HID output report：Report ID `0x30` + `F8 09 07 01 01 00 00`，方向盤 detach 後以 c266 重新枚舉 | high | new-lg4ff `hid-lg4ff.c:416-421, 1576-1598`、PR #50 |
| 設定旋轉範圍 40–900° | **可行**，`F8 81 lo hi 00 00 00` | high | new-lg4ff `lg4ff_set_range_g25`、Logitech FF Protocol V1.6 Table 64/65 |
| RPM LED | **可行**，`F8 12 <5-bit pattern> 00 00 00 00`（5 顆 LED 左右鏡射成對） | high | Logitech V1.6 Table 62/63、new-lg4ff `lg4ff_set_leds` |
| Auto-center spring 開/關/強度 | **可行**，`F5`、`FE 0D k1 k2 clip` + `14` | high | mainline/new-lg4ff `lg4ff_set_autocenter_default` |
| Constant / spring / damper / periodic / ramp force | **可行**，classic slot-based 指令（4 個 slot），periodic 與 ramp 由 host 端每 2 ms 合成為 constant force | high | new-lg4ff `lg4ff_update_slot`、SDL3 `SDL_hidapihaptic_lg4ff.c` |
| Friction (type 0x0E) | **未確認**：Logitech 文件僅列 DFP/G25/DFGT/G27；new-lg4ff 對 G923 給 caps=0，用 damper 模擬 | medium | Logitech V1.6 Table 51、new-lg4ff `hid-lg4ff.c:244-255, 1103-1110` |
| `ForceFeedback.framework` plug-in（讓原生遊戲自動獲得 FFB） | **不可行（SIP on）**。framework 內部 `DoesServiceHaveUUID` 硬編碼 `/System/Library/Extensions/` 前綴，plug-in 執行檔必須實體存在於 sealed read-only system volume | high（兩個獨立的 disassembly 交叉驗證） | 本機 ForceFeedback.framework 1.0.6 反組譯；`csrutil status`、`mount` |
| DriverKit dext | **不建議 / 實務上不可行**：需 Apple 核發 `com.apple.developer.driverkit.transport.usb`（VID 0x046d 非申請者所有）；本機測試需 SIP off；而且即使裝上也救不了 FF plug-in 的路徑限制 | high | Apple DriverKit 文件、`systemextensionsctl developer` 在 SIP on 時拒絕 |
| HID++ (feature 0x8123) FFB | **不適用於 PS 版**：c266/c267 的 HID++ interface 沒有 0x8123/0x8138/0x8139；只有 Xbox 版 (c26e) 用 HID++ FFB | medium-high | mescon FEATURE_MATRIX（硬體枚舉）、ZRtm issue #3、mainline `hid-logitech-hidpp.c` 只綁 c262/c26e |
| TrueForce（遊戲授權的高頻觸覺內容） | **不可行**：遊戲只透過 Windows-only 且驗簽的 `trueforce_sdk_x64.dll` → named pipe → G HUB；沒有 macOS SDK | medium | mescon `docs/TRUEFORCE_PROTOCOL.md`、`sdk/README.md` |
| TrueForce **wire protocol**（自行合成的觸覺串流） | **技術上可行但 v1 不做**：interface 2（usage page 0xFFFD/0xFD01）64-byte report ID 0x01 串流，已在 c266 硬體上驗證；串流期間 classic FFB 會被覆蓋，必須把 classic force 鏡射（取負號）進 `cur` 欄位 | medium | mescon `TRUEFORCE_PROTOCOL.md`、Trueforce-For-All `TrueforceDevice.cs` |
| Wine / CrossOver / GPTK 內的 DirectInput FFB | **可行但需橋接**：Wine 7.0+ 的 dinput 只認 HID PID；macOS `bus_iohid.c` 無 FFB；`bus_sdl.c` 走 SDL2 haptic → ForceFeedback.framework（死路）。可行路線：dinput8.dll proxy + local socket bridge（CrossFFB 模式），或替換 bottle 內的 libSDL2 dylib（g29-mac shim 模式） | high | wine `dlls/winebus.sys/bus_iohid.c`, `bus_sdl.c`, `main.c`；CrossFFB、g29-mac 原始碼 |
| SDL3 原生遊戲 | **可行**：SDL3 ≥ 3.4.0 已內建 lg4ff hidapi 驅動（macOS 預設啟用、非獨占開啟），但 `supported_device_ids` 不含 c266/c267 → 需 patch/upstream | high | SDL3 `SDL_hidapi_lg4ff.c:34-49`、`SDL_hidapihaptic_lg4ff.c:39-55` |
| 原生 ETS2/ATS | 遊戲本身在 Mac 無 FFB；可用 SCS telemetry plugin 自行產生力回饋（fffb / ets2-g29-ffb-macos 模式） | medium | Steam 討論、Torqer 指南 |
| GameController.framework `GCRacingWheel` | **只有輸入**，沒有 haptics API；其 `acquireDevice` 是 exclusive，可能與我們的 daemon 搶裝置 | high | macOS 26 SDK `GCRacingWheel.h` |

### 1.3 建議路線（詳見第 6 節）

1. **核心**：user-space daemon `g923d`（LaunchAgent）負責：偵測 c267 → 送 mode switch；在 c266 上設定 range / LED / autocenter；提供 local socket API 給橋接層；內建 lg4ff effect engine（2 ms tick）。
2. **消費端橋接**（依覆蓋率排序）：(a) patch SDL3 lg4ff 驅動加入 c266/c267 並 upstream；(b) Wine/CrossOver 用 dinput8 proxy DLL + bridge；(c) SCS telemetry plugin 給原生 ETS2/ATS。
3. **CLI 工具** `g923ctl` 作為第一個可測試的交付物。
4. **Fallback**：若 SDL3 patch 路線在某個遊戲不適用，改用 bottle 內替換 `libSDL2-2.0.0.dylib` 的 shim（實作 `SDL_Haptic*` 17 個 entry point 轉呼叫 daemon）。ForceFeedback.framework plug-in 只在使用者願意關 SIP 時才是選項，本專案不採用。

---

## 2. 現況：為什麼 G923 在 Mac 上「不能用」

### 2.1 開箱即用的部分

- 方向盤插上 Mac 後以 USB `046d:c267`（PS mode）枚舉；三個 HID interface 都會被 `AppleUserUSBHostHIDDevice` 接管，IOHIDManager 看得到，Game Controller 相關 kext 沒有針對 0x046d 的 personality（本機 `IOGameControllerFamily` / `AppleSyntheticGameController` 的 plist 檢查過，只有 PSVR2 與 generic）。
- 在 c267 模式下 Linux 使用者回報「steering and most of the buttons does not work」（PR #50）；ZRtm issue #2 的 usbhid-dump 顯示真正的軸資料塞在 64-byte report ID 0x01 的 vendor 區（offset ~43 起，同 GIMX `g29_ps4.h` 的 PS4 layout），HID 宣告的 X/Y/Z/Rz 固定 0x80。所以**未切換前，一般遊戲看到的是一顆「沒反應」的 gamepad**。這解釋了大量論壇回報。
- 切到 c266 後，interface 0 變成標準 Joystick collection（12-byte report：16-bit 方向盤、8-bit 三踏板、25 顆按鍵、hat），任何 HID/GameController/SDL 程式都能直接讀。

### 2.2 G HUB 在 Mac 上做了什麼、沒做什麼

- Logitech 官方規格頁：「Mac OS X 10.10.x or later (G HUB on Mac is used only to update Firmware)」。
- 本機 `/Applications/lghub.app/Contents/Library/SystemExtensions/` 只有 `com.logi.ghub.hidfilter.dext`（personality 只 match `idVendor 1133 / idProduct 0xC24A、0xC537`，即滑鼠）與 `com.logi.ghub.audiooverride.dext`（耳機）。整個 lghub.app 內 `grep -rl IOCFPlugInTypes` 與 FF type UUID 均無結果。
- 結論：G HUB 不會偵測、切換或驅動方向盤；Windows 上 G HUB 也是用 classic `11 08 <level>` 指令驅動 c266（Trueforce-For-All 的 USB capture），TrueForce 設定頁對方向盤**不送任何指令**（PR #50）。
- 社群建議（Torqer 指南）：**不要裝 G HUB**，避免它的 HID filter 干擾；本機證據顯示它不 match 方向盤，但保留為風險。

### 2.3 Logitech 歷史上的 Mac FFB 路徑已死

- LogitechForceFeedback.kext（32-bit only ForceFeedback plug-in，OS X ≤ 10.7）；LGS for Mac 8.87.92/9.02.22（2016–2018，Intel，附 `LogiWheelDriver.kext` + `LogiWheelForceFeedback.kext`，論壇回報 Ventura 需關 SIP 且仍無 FFB）。Apple Silicon + SIP 下完全不可用。
- Feral 的 FreeTheWheel（2012）證明 user-space `IOHIDDeviceSetReport` 送 `F8 xx` 指令的可行性，但 FFB 仍靠 kext。

### 2.4 為什麼「正統」的 ForceFeedback.framework 路線走不通（決定性發現）

兩個研究員各自反組譯本機 `/System/Library/Frameworks/ForceFeedback.framework` (v1.0.6, arm64e)，結論一致：

- `FFIsForceFeedback()` 與 `FFCreateDevice()` 在呼叫 `IOCreatePlugInInterfaceForService` **之前**先呼叫內部函式 `DoesServiceHaveUUID`。
- `DoesServiceHaveUUID` 讀取 service 的 `IOCFPlugInTypes` 字典 → 找 key `F4545CE5-BF5B-11D6-A4BB-0003933E3E3E` → 取得 bundle 名稱 → **無條件** `CFStringAppendCString("/System/Library/Extensions/")` + 名稱 + `/Contents/MacOS/<CFBundleExecutable>` → `stat()` **且** `dlopen()` 都成功才回傳 1。沒有 leading-`/` 絕對路徑判斷、沒有 `/Library/Extensions` fallback（framework 的 `__cstring` 段只有這一個字面值）。失敗則回傳 `0xE00002BE` (kIOReturnUnsupported) / `0x80000003` (FFERR_INVALIDPARAM)。
- 本機 `mount`：`/dev/disk3s1s1 on / (apfs, sealed, local, read-only)`；`touch /System/Library/Extensions/x` → Read-only file system。
- IOKitUser 的 `IOFindPlugIns` 雖然接受絕對路徑與 `/Library/Extensions`，但它只在上述 gate 通過之後才執行，無法被利用。
- 就算把 `IOCFPlugInTypes` 塞進 registry（IOHIDDevice 系列節點的 `setProperties` 不擋這個 key；dext personality 也能帶）也沒用，bundle 位置的 gate 仍然擋住。
- 系統內沒有任何內建的 FF-type plug-in 可以借用（`grep -rli F4545CE5...` 掃 /System/Library/Extensions、DriverExtensions、/Library 皆無）。

因此：**在 SIP on 的機器上，任何呼叫 `FFCreateDevice` 的程式（SDL2/SDL3 的 darwin haptic backend、舊版 Wine、Feral 若有用）都永遠拿不到 G923 的 FFB。我們必須繞過這個 framework。**

---

## 3. G923 PS 版通訊協定速查

> 本節所有位元組均以原始碼／官方 PDF 交叉驗證；標記「unconfirmed」者需實機確認。Logitech Force Feedback Protocol V1.6 PDF 本地副本：`scratchpad/research/protocol_ps/doc/Logitech_FF_Protocol.pdf`（sha256 `9ea8ae6cf3b4b9621361a1a6ae405722d2ce85628ec449e6b8eac7235d46f63d`）。

### 3.1 USB ID 與模式

| VID:PID | 意義 | bcdDevice | 備註 | Confidence |
|---|---|---|---|---|
| `046d:c267` | G923 PS 版，**PS mode**（插上時預設） | `0x3800` | 3 個 HID interface；IF0 = Gamepad 64-byte report 0x01（軸資料在 vendor 區）；必須切換才能用 | high |
| `046d:c266` | G923 PS 版，**Classic / PC mode**（切換後） | `0x3800`（**不變**） | 與 G29 c24f 相同的 classic protocol；產品字串不變 "G923 Racing Wheel for PlayStation 4 and PC" | high |
| `046d:c26d` | G923 Xbox 版，Xbox GIP mode | `0x3902` 觀察值 | 不適用本專案 | medium |
| `046d:c26e` | G923 Xbox 版，PC/HID++ mode | — | HID++ 0x8123 FFB，mainline Linux 支援；不適用本專案 | high |
| `046d:c24f` | G29（PS3 位置 / classic native） | `0x89xx` | 對照組，protocol 與 c266 相同 | high |
| `046d:c260` | G29 PS4 mode | — | 對照組 | high |
| `046d:c294` | G29 開機時的 Driving Force 相容模式 | `0x1350`/`0x89xx` | G923 **沒有**這個機制 | high |
| `046d:c261 / c262` | G920（初始 / active） | — | HID++，不適用 | high |

重要差異（相對 G29）：
1. 識別**只能靠 PID**（bcdDevice 兩種模式都是 0x3800）。
2. **沒有相容模式**（G29 可切成 DF-EX/DFP/DFGT/G25/G27；G923 送這些指令會以同樣的 c266 重新連線，什麼都不變）。
3. 沒有實體 PS3/PS4 切換開關（Logitech 規格頁與手冊未列；此點 verification 標記為 unverifiable，但所有 Linux 回報都是固定以 c267 枚舉）。
4. 每次 power cycle / USB reset 都回到 c267（Linux 每次 probe 都重送 switch）。`F8 0A 00`（revert identity = 0）是否能讓 c266 持久化：**unconfirmed**。

### 3.2 USB / HID interface 拓撲

**c267（PS mode）— 來源：new-lg4ff issue #44 `lsusb -v`、ZRtm issue #2 `usbhid-dump`（唯一公開的完整 descriptor）**

| IF | bcdHID | rdesc 長度 | Endpoints | 內容 |
|---|---|---|---|---|
| 0 | 1.10 | 193 B | EP `0x03` OUT 64 B bInterval 5、EP `0x84` IN 64 B bInterval 5 | 三個 top-level collection：(a) Gamepad (page 0x01, usage 0x05) Input report ID `0x01` 64 B、Output report ID `0x05` 31 B、Feature `0x03` 47 B；(b) vendor page 0xFFF0 usage 0x40：Feature `0xF0`(63 B)、`0xF1`(63 B)、`0xF2`(15 B)、`0xF3`(7 B)；(c) **Joystick (page 0x01, usage 0x04)：Output report ID `0x30`，vendor page 0xFF01 usage 0x02，7 bytes** ← mode switch 要寫這個；Feature `0x31` 126×16-bit |
| 1 | 1.11 | 54 B | EP `0x82` IN 20 B bInterval 5（**沒有 OUT**） | HID++：report ID `0x10` short（6 payload）、`0x11` long（19 payload）in/out；kernel log "HID++ 4.2 device connected" |
| 2 | 1.11 | 30 B | EP `0x81` IN 64 B、EP `0x01` OUT 64 B bInterval 1 | vendor page `0xFFFD` usage `0xFD01`，report ID `0x01`，63 B in / 63 B out = **TrueForce 串流** |

c267 IF0 完整 descriptor（usbhid-dump）：
```
05 01 09 05 A1 01 85 01 09 30 09 31 09 32 09 35 15 00 26 FF 00 75 08 95 04 81 02
09 39 15 00 25 07 35 00 46 3B 01 65 14 75 04 95 01 81 42 65 00
05 09 19 01 29 0E 15 00 25 01 75 01 95 0E 81 02
06 00 FF 09 20 75 06 95 01 81 02
05 01 09 33 09 34 15 00 26 FF 00 75 08 95 02 81 02
06 00 FF 09 21 95 36 81 02
85 05 09 22 95 1F 91 02
85 03 0A 21 27 95 2F B1 02 C0
06 F0 FF 09 40 A1 01 85 F0 09 47 95 3F B1 02 85 F1 09 48 95 3F B1 02
85 F2 09 49 95 0F B1 02 85 F3 0A 01 47 95 07 B1 02 C0
05 01 09 04 A1 01 85 30 06 01 FF 09 02 95 07 91 02
85 31 95 7E 75 10 05 10 19 01 2A FF FF B1 40 C0
```
IF1：`06 00 FF 09 01 A1 01 85 10 75 08 95 06 15 00 26 FF 00 09 01 81 00 09 01 91 00 C0 06 00 FF 09 02 A1 01 85 11 75 08 95 13 15 00 26 FF 00 09 02 81 00 09 02 91 00 C0`
IF2：`06 FD FF 0A 01 FD A1 01 85 01 15 00 26 FF 00 75 08 95 3F 09 01 81 00 95 3F 09 01 91 00 C0`

**c266（Classic mode）**
- 也是 **3 個 HID interface**（ZRtm issue #3 的 dmesg：`C266.001C Joystick /input0`、`.001D /input1`、`.001E /input2`）。IF0 = Joystick（page 0x01 usage 0x04，HID 1.11）；IF1/IF2 descriptor **未公開**（推測仍是 HID++ 0xFF00 與 TrueForce 0xFFFD；mescon 的 Rust 碼以 descriptor 前綴辨識：`05 01 09 04` = Joystick、`06 00 FF` = HID++、`06 FD FF` = TrueForce）。
- **endpoint 編號在 c266 可能與 c267 不同**（Trueforce-For-All 對 c266 的描述是 "Trueforce ep3 ... non-Trueforce FFB on ep01"，剛好與 c267 相反）。**不要硬編 endpoint**；一律用 report-level API（`IOHIDDeviceSetReport`）並以 usage page/usage 選 interface。
- Linux 驅動只綁 interface 0（`hid-lg.c:770-777`）；PR #50：「Attempts to initialize all interfaces all ends up in kernel panics」。我們的 daemon 也**只開 IF0**，不要碰 IF1/IF2。

c266 IF0 report descriptor（lgff_wheel_adapter `usb_descriptors.h:515-580`，作者轉錄，經三個獨立按鍵表交叉驗證）：
```
05 01 09 04 A1 01
  09 39 15 00 25 07 35 00 46 3B 01 65 14 75 04 95 01 81 42      ; hat 4-bit, null state
  05 09 19 01 29 19 65 00 25 01 45 01 75 01 95 19 81 02          ; 25 buttons
  75 01 95 03 81 03                                              ; 3 pad bits
  05 01 09 30 27 FF FF 00 00 47 FF FF 00 00 75 10 95 01 81 02    ; X 16-bit, max 65534
  09 32 09 35 09 31 26 FF 00 46 FF 00 75 08 95 03 81 02          ; Z, Rz, Y 8-bit (gas, brake, clutch)
  06 00 FF 09 00 09 01 95 02 81 02                               ; 2 vendor bytes (shifter X, Y)
  25 01 45 01 19 02 29 09 75 01 95 08 81 02                      ; 8 vendor status bits
  06 01 FF 09 02 26 FF 00 46 FF 00 95 10 75 08 91 02             ; OUTPUT 16 bytes, no report ID
C0
```

### 3.3 Mode switch（c267 → c266）

| 指令 | Report ID | Payload (7 B) | 說明 | Confidence |
|---|---|---|---|---|
| **G923 PS → Classic（採用）** | `0x30` | `F8 09 07 01 01 00 00` | EXT_CMD 0x09 Change Device Mode，DEVICE=0x07 (G923)，DETACH=1。送到 IF0。方向盤 detach、以 c266 重新枚舉 | high |
| Logitech V1.6 Table 115 版本 | `0x30` | `F8 09 05 01`（+ 補零） | 官方文件的寫法（DEVICE=0x05 = G29 碼）；new-lg4ff 作者回報**不能用** | high（文件內容）/ 該指令本身 unconfirmed |
| new-lg4ff 另一組（未用於自動切換） | — | `F8 0A 00 00 00 00 00` 然後 `F8 09 07 01 01 00 00` | ext09 形式（先 revert identity=0） | high（存在）/ 效果 unconfirmed |
| G29 對照 | 無 | `F8 0A 00 00 00 00 00`、`F8 09 05 01 01 00 00` | c294 → c24f；**對 G923 無效** | high |
| Xbox 版對照 | — | `0F 00 01 01 42` | c26d → c26e，非 HID output report（需 IOUSBHost）；不適用 | medium |

macOS 送法（描述子一致的寫法）：
```c
uint8_t buf[8] = {0x30, 0xF8, 0x09, 0x07, 0x01, 0x01, 0x00, 0x00};
IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x30, buf, sizeof buf);
```
- macOS `IOHIDDevice.h` 規定：裝置使用 report ID 時，buffer 第一個 byte 也要放 report ID；`IOHIDLibUserClient::setReport` 把 buffer 原封不動交給 `fNub->setReport(mem, type, reportID)`，不會幫你加/去 ID，也不驗證長度。
- Linux 實際上送的是 32 bytes（`30 F8 09 07 01 01 00 00` + 24 個 0，因為 new-lg4ff 借用 IF0 第一個 output report（ID 5, 31 B）再把 id 改成 0x30）；usb_modeswitch / LogiWheelHost / lgff_wheel_adapter 送 8 bytes。兩者都成功 → 韌體忽略尾端多餘 bytes。
- 選 device：c267 的 IF0 有三個 top-level collection，macOS 為每個 USB interface 建立一個 `IOHIDDevice`，`kIOHIDDeviceUsagePairsKey` 會列出 (0x01,0x05)、(0xFFF0,0x40)、(0x01,0x04)；用 `kIOHIDDeviceUsagePairsKey` 含 (0x01,0x04) 或 `kIOHIDMaxOutputReportSizeKey`=31 來辨識 IF0（**macOS 是否進一步把 collection 拆成多個 IOHIDDevice：unconfirmed，實機用 `ioreg -l -w0 | grep -A30 C267` 看**）。
- 時序：hm0429 切換後等 8 s 讓校正掃描完成；LogiWheelHost 在剛接上時 OUT endpoint 可能未就緒，每 400 ms 重試最多 10 次；某些 Ryzen/xHCI Linux 主機看到約 1 分鐘的 hang（其他驅動抓著 IF1/IF2 時）。daemon 應該：送出 → 等 c267 消失 → 等 c266 出現（timeout 15 s）→ 再等 ~2 s 校正 → 送 init 序列。
- 必須在 c266 送 classic 指令時把 report ID 改回 **0**（new-lg4ff 不重設 `report->id` 是因為裝置會 detach）。

### 3.4 c266 Input report（12 bytes，無 report ID，IF0）

| Byte | Bit | 內容 | 值域 |
|---|---|---|---|
| 0 | 0–3 | Hat | 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW 8=釋放 |
| 0 | 4 | Cross (btn 1) | |
| 0 | 5 | Square (btn 2) | |
| 0 | 6 | Circle (btn 3) | |
| 0 | 7 | Triangle (btn 4) | |
| 1 | 0 | R1 / 右換檔撥片 (btn 5) | |
| 1 | 1 | L1 / 左換檔撥片 (btn 6) | |
| 1 | 2 | R2 (btn 7) | |
| 1 | 3 | L2 (btn 8) | |
| 1 | 4 | Share (btn 9) | |
| 1 | 5 | Options (btn 10) | |
| 1 | 6 | R3 (btn 11) | |
| 1 | 7 | L3 (btn 12) | |
| 2 | 0–5 | H 排檔 1–6 檔 (btn 13–18) | |
| 2 | 6 | 排檔 R (btn 19) | |
| 2 | 7 | `+` (btn 20) | |
| 3 | 0 | `−` (btn 21) | |
| 3 | 1 | 旋鈕順時針 (btn 22) | |
| 3 | 2 | 旋鈕逆時針 (btn 23) | |
| 3 | 3 | Enter / 旋鈕按下 (btn 24) | |
| 3 | 4 | PS (btn 25) | |
| 3 | 5–7 | constant padding | |
| 4–5 | — | 方向盤角度 u16 LE | 0 = 最左，~0x7FFF/0x8000 = 中央，65534 = 最右 |
| 6 | — | 油門 (usage Z) | 0xFF 放開，0x00 踩到底（**反向**） |
| 7 | — | 煞車 (usage Rz) | 同上 |
| 8 | — | 離合器 (usage Y) | 同上 |
| 9 | — | 排檔桿 X（vendor 0xFF00 usage 0x00） | ~0x80 idle |
| 10 | — | 排檔桿 Y（vendor 0xFF00 usage 0x01） | ~0x80 idle |
| 11 | 1 | 踏板未連接 | G923 實測 |
| 11 | 2 | 電源已連接（或「已校正」，不確定） | G923 實測 |
| 11 | 6 | 排檔桿被壓下（倒檔閘） | |
| 11 | 4, 7 | G923 上恆為 1，意義未知 | |
| 11 | 0, 3, 5 | 未知 | |

按鍵 mask（LogiWheelHost 在 c266 實機確認，`buttons = b0 | b1<<8 | b2<<16 | b3<<24`）：X 0x10、Square 0x20、Circle 0x40、Triangle 0x80、R_PADDLE 0x100、L_PADDLE 0x200、R2 0x400、L2 0x800、SHARE 0x1000、OPTIONS 0x2000、R3 0x4000、L3 0x8000、PLUS 0x800000、MINUS 0x01000000、DIAL_CW 0x02000000、DIAL_CCW 0x04000000、BACK 0x08000000、PS 0x10000000、HAT_MASK 0x0F。

combined pedals 模擬（new-lg4ff `lg4ff_raw_event`）：`combine=1`：`rd[6] = (0xFF + rd[6] - rd[7]) >> 1; rd[7] = 0x7F`；`combine=2` 用 clutch `rd[8]`。

### 3.5 c266 Output report 與 macOS 傳輸細節

- Descriptor：vendor page 0xFF01 usage 0x02，Report Count 16 × 8 bit = **16 bytes，無 report ID**。classic 7-byte 指令放 bytes 0–6，其餘補 0。
- Linux `hid_hw_request(SET_REPORT)` 實際透過 usbhid 走 **interrupt OUT**（interface 有 OUT endpoint 時），送整個 16-byte report、無 ID byte；SDL 經 hidapi 送 7 bytes 也成功（Linux）。
- macOS 實測（三個 G29 專案，c24f）：`IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0, buf, 7)` 回 `0xE0005000` (`kUSBHostReturnPipeStalled`)；補到 `kIOHIDMaxOutputReportSizeKey`（G29 = 16）即成功。fffb 在 c266 G923 用 8 bytes 且 `SeizeDevice`、reportID 傳 `time(NULL)`（bug）—作者稱可用但另一位 G923 用戶回報無 FFB。**規則：讀 `kIOHIDMaxOutputReportSizeKey`，補零到該長度，reportID=0。**
- macOS 的 USB HID transport（`AppleUserUSBHostHIDDevice`）是閉源；`IOUserUSBHostHIDDevice.iig` 同時文件化 interrupt-pipe (`CompleteOutputReport`) 與 control-pipe (`CompleteOutputRequest`) 兩條路；`IOHIDDeviceSetReport(Output)` 走哪條 **unconfirmed**（FreeTheWheel 用亂數 reportID 仍可用，是 interrupt pipe 的旁證）。
- 開啟方式：`IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone)`（shared，讓遊戲 / GameController 仍能讀輸入）。從 Terminal 執行需 **Input Monitoring** TCC 授權（`kIOReturnNotPermitted 0xE00002E2` 時提示使用者）。
- 錯誤碼備查：`kIOReturnExclusiveAccess 0xE00002C5`、`kIOReturnUnsupported 0xE00002C7`、`kIOReturnNotPrivileged 0xE00002C1`、`kIOReturnNotPermitted 0xE00002E2`、`kUSBHostReturnPipeStalled 0xE0005000`。

### 3.6 Classic 7-byte 指令格式（Logitech FF Protocol V1.6 Table 1–3）

```
byte 0 : [F3 F2 F1 F0 | CMD(4 bits)]   F0..F3 = force slot 1..4 → 0x10, 0x20, 0x40, 0x80
byte 1 : FORCE_TYPE（download/play/refresh 時）或 CMD_PARAM
byte 2-6 : 參數（未用填 0x00）
```

| CMD | 意義 | 備註 |
|---|---|---|
| 0x0 | Download Force | byte1 = force type |
| 0x1 | Download and Play | 最常用：`0x11`=slot1, `0x21`=slot2, `0x41`=slot3, `0x81`=slot4 |
| 0x2 | Play | |
| 0x3 | Stop | `0x13/0x23/0x43/0x83`；`0xF3` = 全部停止 |
| 0x4 | Default Spring On | `0x14`（slot bit 只是慣例填 F0） |
| 0x5 | Default Spring Off | `0xF5` |
| 0x8 | F bits 全 0 = Normal Mode；F bits 全 1 (`0xF8`) = **Extended Command** | |
| 0x9 | Set LED（byte1 = 8 LED bits，舊機種） | G923 用 ext 0x12 |
| 0xA | Set Watchdog（byte1 = main-loop 數，0 = off） | |
| 0xB | Raw Mode | |
| 0xC | Refresh Force（更新播放中的力） | `0x1C/0x2C/0x4C/0x8C` |
| 0xD | Fixed Time Loop（byte1: 0 = as fast as possible, 1 = 每 2 ms 更新） | init 時送 `0D 00` 或 `0D 01` |
| 0xE | Set Default Spring（byte1 = 0x0D 高解析 autocenter；byte2 K1, byte3 K2, byte4 CLIP） | `0xFE 0D ...` |
| 0xF | Set Dead Band（byte1 0/1） | |

力的等級：單一 byte，`0x00` = 朝「0」端最大力（順時針），`0x7F/0x80` = 無力，`0xFF` = 朝「255」端最大力（逆時針）；方向盤位置 0 = 左、255 = 右。

### 3.7 Force types 與參數配置（V1.6 Table 23–51）

| Type | 名稱 | byte2 | byte3 | byte4 | byte5 | byte6 | G923 狀態 |
|---|---|---|---|---|---|---|---|
| 0x00 | Constant | level F0 | level F1 | level F2 | level F3 | 0 | ✔（new-lg4ff slot 0） |
| 0x01 | Spring | D1 dead-band 下限 | D2 上限 | `[0 K2(3b) 0 K1(3b)]` | `[000 S2 000 S1]` | CLIP | ✔（低解析） |
| 0x02 | Damper | K1(3b) | S1(bit0) | K2(3b) | S2(bit0) | 0 | ✔ |
| 0x03 | Auto-centering spring | K1 | K2 | CLIP | 0 | 0 | ✔ |
| 0x04/0x05 | Sawtooth up/down | L1 max | L2 min | L0 initial | 0 | `[T3(4b) INC(4b)]` | host 端合成較佳 |
| 0x06 | Trapezoid | L1 | L2 | T1 | T2 | `[T3 S]` | fffb 有用 |
| 0x07 | Rectangle | L1 | L2 | T1 | T2 | P | |
| 0x08 | Variable（只能 F0/F2；byte0=`[0 F2 0 F0 CMD]`） | L1（force 0 初值） | L2（force 2 初值） | `[T1(4b) S1(4b)]` | `[T2 S2]` | `[000 D2 000 D1]` | ✔ S=0 時即 constant；**G HUB/mainline 用 `11 08 level 80`** |
| 0x09 | Ramp | L1 | L2 | bit0 D | `[T S]` | 0 | host 端合成 |
| 0x0A | Square wave | A | T low byte | T high byte | N | 0 | G923 **無反應**（PR #50） |
| 0x0B | High-res Spring | D1[10:3] | D2[10:3] | `[K2(4b) K1(4b)]` | `[D2[2:0] S2 D1[2:0] S1]` | CLIP | ✔（new-lg4ff slot 1–3） |
| 0x0C | High-res Damper | K1(4b) | S1 | K2(4b) | S2 | CLIP（文件僅 DFP；new-lg4ff 照送） | ✔ |
| 0x0D | High-res Auto-center | K1(4b) | K2(4b) | CLIP | 0 | 0 | ✔（經 `FE 0D`） |
| 0x0E | Friction | K1 | K2 | CLIP | `[000 S2 000 S1]` | 0 | **unconfirmed**（文件僅 DFP/G25/DFGT/G27） |

K 係數：低解析 0..7 → 1/4,1/2,3/4,1,3/2,2,3,4 × offset（GIMX 以 {1/16,1/8,3/16,1/4,3/8,1/2,3/4,1} 正規化；順序才重要）；高解析 0..15 線性。

### 3.8 FFB 指令總表（可直接編碼；每列均為 7 bytes，c266 時 reportID=0，補零到 16 bytes）

| 功能 | Bytes (hex) | 說明 / 來源 |
|---|---|---|
| Constant force, slot 1, download+play（new-lg4ff 型式） | `11 00 LL 00 00 00 00` | `LL = ((clamp_s16(level) + 0x8000) >> 8)`，0x80 = 無力；slot n 的 level 放 byte `2+n` |
| Constant force, slot 1, refresh | `1C 00 LL 00 00 00 00` | 播放中更新 |
| Constant force, slot 1, stop | `13 00 00 00 00 00 00` | |
| Constant force（G HUB / mainline / GIMX 型式） | `11 08 LL 80 00 00 00` | variable type，byte3 0x80 = force-2 初值，S=0 不 ramp；Windows capture 證實 G HUB 對 c266 就送這個。停止：`13 00 00 00 00 00 00` |
| CrossFFB 在 G29 上驗證的變體 | `11 00 LL 80 80 80 00` | 未用 slot 填 0x80（安全預設） |
| High-res spring, slot 2, download+play | `21 0B d1h d2h KK SS CC` | `d1 = SCALE_U16((d1+0x8000)&0xFFFF, 11)`（11-bit）、`d1h = d1>>3`；`KK = (SCALE_COEFF(k2,4)<<4) | SCALE_COEFF(k1,4)`；`SS = ((d2&7)<<5) | ((d1&7)<<1) | (s2<<4) | s1`；`CC = clip>>8`。refresh `2C`, stop `23` |
| High-res damper, slot 3 | `41 0C K1 S1 K2 S2 CC` | `K = SCALE_COEFF(k,4)`（4-bit），S = 係數負號，`CC = clip>>8`。refresh `4C`, stop `43` |
| Friction, slot 4（若韌體支援） | `81 0E K1 K2 CC SS 00` | `K = SCALE_COEFF(k,8)`，`SS = (s2<<4)|s1`。refresh `8C`, stop `83` |
| Stop all slots | `F3 00 00 00 00 00 00` | deinit |
| Fixed time loop off / on | `0D 00 00 00 00 00 00` / `0D 01 00 00 00 00 00` | init（SDL3 送 0，new-lg4ff 預設 0） |
| Autocenter OFF | `F5 00 00 00 00 00 00` | 開機時預設 spring 是開的，init 一定先送 |
| Autocenter 設定 | `FE 0D AA AA BB 00 00` | 見下方公式 |
| Autocenter ON | `14 00 00 00 00 00 00` | 在 `FE 0D` 之後送 |
| Rotation range | `F8 81 lo hi 00 00 00` | 40 ≤ range ≤ 900；900 = `F8 81 84 03 00 00 00`，270 = `F8 81 0E 01 00 00 00` |
| RPM LEDs | `F8 12 PP 00 00 00 00` | `PP` bits0–4，每 bit 點亮左右各一顆；0x1F 全亮；SDL 的 1..5 顆對應 1,3,7,15,31 |
| Mode switch（僅 c267，reportID 0x30） | `F8 09 07 01 01 00 00` | 見 3.3 |
| Revert identity | `F8 0A 00 00 00 00 00` | 對 G923 效果 unconfirmed |

Autocenter 強度公式（mainline & new-lg4ff `lg4ff_set_autocenter_default`，m = 0..65535）：
```
if m <= 0xAAAA: a = 0x0C*m;                     b = 0x80*m
else:           a = 0x0C*0xAAAA + 0x06*(m-0xAAAA); b = 0x80*0xAAAA + 0xFF*(m-0xAAAA)
a >>= 1   # 非 MOMO 方向盤
AA = a / 0xAAAA ; BB = b / 0xAAAA
send FE 0D AA AA BB 00 00 ; send 14 00 00 00 00 00 00
```
Macro（new-lg4ff `hid-lg4ff.c:79-83`）：
```
CLAMP_U16(x) = min(x, 0xFFFF)
CLAMP_S16(x) = clamp(x, -0x8000, 0x7FFF)
SCALE_VALUE_U16(x, bits) = CLAMP_U16(x) >> (16 - bits)
SCALE_COEFF(x, bits) = SCALE_VALUE_U16(abs(x) * 2, bits)
TRANSLATE_FORCE(x) = (CLAMP_S16(x) + 0x8000) >> 8
spring: k1<2048 → d1=0 else k1-=2048 ; k2<2048 → d2=2047 else k2-=2048   (小 dead-zone 技巧)
```

### 3.9 new-lg4ff 在 c266 上的 init 序列（經 verification 修正）

```
F5 00 00 00 00 00 00      ; autocenter off（lg4ff_init 的 set_autocenter(dev,0)）
F8 81 84 03 00 00 00      ; range 900
0D 00 00 00 00 00 00      ; fixed loop（module 參數 fixed_loop=0 → byte1 0）
11 00 80 00 00 00 00      ; slot0 = constant, zero force, download+play（不是 stop）
23 00 00 00 00 00 00      ; slot1 stop
43 00 00 00 00 00 00      ; slot2 stop
83 00 00 00 00 00 00      ; slot3 stop
```
執行模型：hrtimer 每 2 ms（`DEFAULT_TIMER_PERIOD`）重算所有 effect；slot 0 = constant + periodic + ramp 的總和（envelope、方向、gain 都在 host 算）；slot 1–3 = 最多三個 condition effect（spring 0x0B / damper 0x0C / friction 0x0E；G923 caps=0 → friction、inertia 轉成 damper）；每個 slot 只在 bytes 有變化時才送；每個 tick 最多 4 個指令。gain = master_gain × effect gain / 0xFFFF 在 host 套用。SDL3 的 `SDL_hidapihaptic_lg4ff.c` 是這段邏輯的 user-space port（slot 0 CONSTANT、1 SPRING、2 DAMPER、3 FRICTION，`SDL_Delay(2)`），可直接移植到 Swift/C。

### 3.10 HID++ 備註（PS 版）

- Logitech V1.6 §2.2：HID++ 只用於 G920 / G923 Xbox 的 FFB；對 G923 PS 只用於 RPM LED feature `x807A`，且「the wheels must be put in to PC/classic mode」。
- c267 IF1 是 HID++ 4.2（report 0x10/0x11；device index 0xFF；`[id][0xFF][feature idx][fn<<4 | swid][params]`；IRoot getFeature = feature 0 fn 0，params {page>>8, page&0xFF}）。因為 IF1 **只有 IN endpoint**，HID++ 寫入只能走 control SET_REPORT。
- 硬體枚舉（mescon，2026-08-08）：c266 回報 ~21 個 feature，含 `0x807A`@0x11、`0x80A3`@0x12、`0x80D0`@0x13、`0x8120`@0x0F、`0x8127`@0x10、`0x8122`、`0x8124`；**沒有** `0x8123` FORCE_FEEDBACK、`0x8138` OPERATING_RANGE、`0x8139` TRUE_FORCE。JacKeTUs 也回報 0x8123/0x8127 在 c266 上做不起來。
- 結論：PS 版的 range / FFB / LED 一律走 classic；HID++ 對本專案唯一可能的用途是 `x807A` LED（但 classic `F8 12` 已足夠），v1 完全不碰 IF1。
- Xbox 版 (c26e) 的 HID++ 0x8123 指令集（GET_INFO 0x01、RESET_ALL 0x11、DOWNLOAD_EFFECT 0x21、SET_EFFECT_STATE 0x31、DESTROY_EFFECT 0x41、GET/SET_APERTURE 0x51/0x61、GET/SET_GLOBAL_GAINS 0x71/0x81；20-byte report 0x11 走 interrupt OUT；feature index 0x0b、64 slots）僅供日後擴充參考，本文不展開。

### 3.11 TrueForce 備註

- 不是 HID++ feature、不是 classic force type：是 IF2（usage page 0xFFFD / usage 0xFD01）上約 1000 pkt/s 的 64-byte output report 串流（report ID 0x01）。
- 封包：`[0]=0x01, [1..3]=0, [4]=type, [5]=rolling seq`；type 0x01 串流：`[6..7]`、`[8..9]` = `cur` 扭矩目標 u16 LE offset-binary（0x8000 = 0），`[10]` = 新樣本數（4），`[11]` = 0x0D（有樣本時；錯了整個 window 被丟棄），`[12..63]` = 13 格 u16 LE 觸覺樣本（L/R 重複），4 kHz 取樣。其他 type：0x03 start、0x04 stop、0x05 參數上傳（48 個 float，全 0 也能動）、0x06 slot 設定、0x07 handshake、0x0E range（G923 忽略）。
- 需先送 68-packet init **兩次**（seq 從 1 重新開始）；同一時間只能有一個 writer（1 ms interval）。
- **G923 關鍵限制**：串流期間馬達只跟 `cur`，classic FFB 失效 → 串流者必須把 classic 淨力鏡射進 `cur`，且經 mescon 在 c266 校正：`cur = 0x8000 + clamp_i16(texture + (−classic_force))`（要**取負號**）。單一來源、未在真實遊戲遙測下驗證。
- 遊戲端 TrueForce 內容經 `trueforce_sdk_x64.dll` → named pipe `logi.trueforce.connect`（驗 Authenticode 簽章「Logitech Inc」）→ G HUB → USB；沒有 macOS SDK。**因此 macOS 上只能自行合成（例如從遙測/引擎轉速產生震動），不可能重現遊戲原生 TrueForce。** v1 不實作。

---

## 4. macOS 力回饋架構

### 4.1 ForceFeedback.framework + IOForceFeedbackLib plug-in 介面（供完整性；本專案不採用）

- Plug-in 是 CFPlugIn (COM)：type UUID `kIOForceFeedbackLibTypeID = F4545CE5-BF5B-11D6-A4BB-0003933E3E3E`（bytes `F4 54 5C E5 BF 5B 11 D6 A4 BB 00 03 93 3E 3E 3E`），interface UUID `kIOForceFeedbackDeviceInterfaceID = 1C7C5850-BB6A-11D6-B75F-003065FBE6B0`（bytes `1C 7C 58 50 BB 6A 11 D6 B7 5F 00 30 65 FB E6 B0`），`kIOCFPlugInInterfaceID = C244E858-109C-11D4-91D4-0050E4C6426F`，`IUnknownUUID = 00000000-0000-0000-C000-000000000046`。
- Vtable（`IOForceFeedbackLib.h:159-206`，8-byte slot 偏移經反組譯確認）：`+0x08 QueryInterface, +0x10 AddRef, +0x18 Release, +0x20 ForceFeedbackGetVersion, +0x28 InitializeTerminate(self, NumVersion, io_object_t, boolean_t begin), +0x30 DestroyEffect, +0x38 DownloadEffect(self, CFUUIDRef type, FFEffectDownloadID*, FFEFFECT*, flags), +0x40 Escape, +0x48 GetEffectStatus, +0x50 GetForceFeedbackCapabilities(FFCAPABILITIES*), +0x58 GetForceFeedbackState, +0x60 SendForceFeedbackCommand(FFCommandFlag), +0x68 SetProperty(FFProperty, void*), +0x70 StartEffect(id, mode, iterations), +0x78 StopEffect(id)`。注意**沒有 GetProperty**：framework 自己回答 `FFDeviceGetForceFeedbackProperty`（行為未文件化）。
- Framework 呼叫順序：`FFCreateDevice` → `IOCreatePlugInInterfaceForService` → `QueryInterface(1C7C5850…)` → `InitializeTerminate(0x01008000, service, begin=1)`（io_object_t 呼叫後即被 release，plug-in 要自己 `IOHIDDeviceCreate`/retain）→ 用 memcpy 快取 `FFCAPABILITIES`。`FFReleaseDevice` → `SendForceFeedbackCommand(STOPALL=2)` → `(RESET=1)` → `InitializeTerminate(begin=0)` → `Release`。`FFEffectStart` 若未下載會先呼叫 `DownloadEffect(…, FFEP_ALLPARAMS=0x3FF)`。`FFDeviceSetForceFeedbackProperty` 先在 framework 驗證 FFGAIN(=1) ≤ 10000、AUTOCENTER(=3) ≤ 1 再呼叫 plug-in。
- 註冊：plug-in bundle Info.plist 需 `CFPlugInDynamicRegistration=NO`、`CFPlugInFactories {factory-UUID: "FactoryFn"}`、`CFPlugInTypes {F4545CE5-…: [factory-UUID]}`；裝置 registry entry 需 `IOCFPlugInTypes {F4545CE5-…: "<bundle path>"}`（歷史上由 kext personality 或 `IOHIDProviderPropertyMerger` 注入）。
- **為什麼被擋**（第 2.4 節）：`DoesServiceHaveUUID` 硬編碼 `/System/Library/Extensions/`，SSV 唯讀。這使得 kext、dext、user-space 注入三條路都無效。
- 其他限制：plug-in 會被 dlopen 進遊戲 process → hardened runtime 的 library validation 要求同 Team ID 或 `com.apple.security.cs.disable-library-validation`。
- 完整參考實作（若日後 SIP 政策改變）：360Controller `Feedback360/`（COM glue、10 ms dispatch timer、波形數學）、`XBOBTFF/FFDriver.cpp`（純 user-space `IOHIDDeviceCreate` + `IOHIDDeviceSetReport` 輸出）。

### 4.2 DriverKit dext

- 機制上可行：dext personality（`IOClass=AppleUserHIDDevice`, `IOUserClass=<class>`, `IOProviderClass=IOUSBHostInterface`）可帶任意 merge 屬性；本機 G HUB / Razer 的 dext 就是這樣掛在 HID 裝置上。
- 但：(1) 需要 `com.apple.developer.driverkit` + `.transport.usb`（列 idVendor/idProduct，0x046d 非申請者所有）+ `.family.hid.device` 由 Apple 核發；(2) 本機測試需 `systemextensionsctl developer on`，SIP on 時該指令直接拒絕；Apple 文件要求 Recovery → Reduced Security → SIP off → `-arm64e_preview_abi`；(3) 就算成功也不能解 FF plug-in 的路徑 gate。
- 結論：**不採用**。dext 能做的（mode switch、range、輸出 report）user-space 全部做得到。

### 4.3 User-space 屬性注入

- `IORegistryEntrySetCFProperties` → `is_io_registry_entry_set_properties`：MACF 檢查 + 可選 allow-list → `entry->setProperties()`。基底 `IORegistryEntry::setProperties` 回 `kIOReturnUnsupported`；`AppleUserHIDDevice::setProperties` 會接受非受限 key（受限：`IOUserClientClass, IOClass, IOProviderClass, IOKitDebug, IOServiceDEXTEntitlements, IOHIDDevicePrivileged`）。`IOCFPlugInTypes` 不在受限清單，可以寫入 → 但因 4.1 的 gate 仍無用。
- 虛擬 HID 裝置（`IOHIDUserDevice`）需要 `com.apple.developer.hid.virtual.device` entitlement → 無法拿來做「虛擬 HID PID 裝置」給 Wine 的 `bus_iohid` 用。

### 4.4 建議：純 user-space HID 客戶端（唯一與 SIP 相容的路線）

```
                    ┌──────────────────────────────────────────────┐
                    │  g923d (LaunchAgent, Swift, arm64)            │
  IOHIDManager ───► │  • discovery: 046d:c267 → send 0x30 switch    │
  (shared open)     │  • 046d:c266 IF0 (usage 0x01/0x04) → init     │
                    │  • range / LED / autocenter / gain 設定        │
                    │  • lg4ff effect engine (2 ms tick, 4 slots)    │
                    │  • local API: UNIX socket + TCP 127.0.0.1      │
                    └───────┬───────────────┬───────────────┬───────┘
                            │               │               │
            ┌───────────────▼──┐  ┌─────────▼────────┐  ┌───▼────────────────┐
            │ SDL3 lg4ff patch │  │ dinput8.dll proxy │  │ SCS telemetry plugin│
            │ (in-process,     │  │ + bridge (Wine /  │  │ (native ETS2/ATS)   │
            │  直接寫 HID)     │  │  CrossOver/GPTK)  │  │                     │
            └──────────────────┘  └───────────────────┘  └─────────────────────┘
```
- 這正是 2024–2026 年所有可用專案（fffb、g29-mac、CrossFFB、ets2-g29-ffb-macos、g923-mac-ffb）與商業軟體（Torqer、CrossWheel）採用的模式；沒有一個需要 kext/dext/SIP 變更。
- 裝置仲裁：daemon 與 SDL3 lg4ff patch 都用 `kIOHIDOptionsTypeNone` 開啟 → 兩者可能同時寫指令互相打架。規則：**同一時間只有一個 FFB writer**。daemon 提供「暫停 engine」API；SDL3 路線的遊戲啟動時透過 socket 通知 daemon 讓出，或 daemon 偵測到別的程式在寫（無法偵測 → 用約定）。
- `GCRacingWheel.acquireDeviceWithError:` 是 exclusive（可能對 IOHIDDevice 做 seize）；原生用 GameController 的遊戲可能讓 daemon 的 `IOHIDDeviceSetReport` 失敗 → 需實機測。

### 4.5 Fallback

1. **主要 fallback（Wine 端）**：若 dinput8 proxy 在某遊戲不相容（例如遊戲用 XInput 或自帶 dinput8），改用 **libSDL2 shim**：替換 bottle/Whisky 內的 `libSDL2-2.0.0.dylib`（x86_64），實作 `SDL_JoystickIsHaptic`、`SDL_HapticOpenFromJoystick`、`SDL_HapticQuery`、`SDL_HapticNumAxes`、`SDL_HapticNewEffect/UpdateEffect/RunEffect/StopEffect/DestroyEffect/SetGain/SetAutocenter/Pause/Unpause/StopAll/GetEffectStatus/Close`、`SDL_JoystickGetType`（回 WHEEL）等，其餘 33 個符號 trampoline 到原始 lib（g29-mac `sdl2-lg4ff-shim.c` 模式，MIT），並設 winebus `Enable SDL=1`（上游預設就是 1）。
2. **主要 fallback（SDL3 端）**：若 upstream SDL3 patch 未被遊戲的 bundled SDL3 採用，改由 daemon 提供 `DYLD_INSERT_LIBRARIES` shim 或請遊戲設 `SDL_JOYSTICK_HIDAPI_LG4FF=0` 並改走 daemon socket。
3. **原生遊戲**：telemetry plugin（ETS2/ATS）；Feral 舊作自帶 wheel 碼，無法介入。
4. **只有使用者願意關 SIP 時**：FF plug-in 放 `/System/Library/Extensions/<x>.kext/Contents/PlugIns/<x>.plugin` + codeless kext personality 注入 `IOCFPlugInTypes`（Apple Silicon 還需 Reduced Security + 允許 kext）。本專案明確**不走**這條。

---

## 5. 誰會用到這個驅動（消費端分析）

| 消費端 | 它怎麼取得 FFB | 在本機的現況 | 我們需要提供的 API | Confidence |
|---|---|---|---|---|
| **SDL2 / SDL3 darwin haptic backend**（`src/haptic/darwin/SDL_syshaptic.c`，`SDL_HAPTIC_IOKIT`） | 對 joystick 的 `IOHIDDevice` service 呼叫 `FFIsForceFeedback`，成功才 `FFCreateDevice` | 永遠失敗（4.1 的 gate）；SDL_iokitjoystick 仍能讀輸入 | 無法提供（需 FF plug-in） | high |
| **SDL3 ≥ 3.4.0 hidapi lg4ff**（`SDL_hidapi_lg4ff.c` + `SDL_hidapihaptic_lg4ff.c`，macOS 預設啟用，`SDL_HINT_JOYSTICK_HIDAPI_LG4FF`） | in-process：`SDL_hid_write` 7-byte classic 指令；effect engine 內建；`SDL_hid_open` 非獨占 | Homebrew sdl3 3.4.16 含此驅動，但 `supported_device_ids` = {c24f, c29b, c299, c29a, c298, c294}，**不含 c266/c267** → G923 落到 IOKit joystick driver，無 FFB | **Patch**：兩個檔案加 `0xc266`、`0xc267`；`SwitchMode` 加 c267 case（reportID 0x30，macOS 需 buffer[0]=0x30）；LED gate 加 G923；按鍵數 25；名稱。並驗證 macOS 上 7-byte `hid_write` 是否 STALL（SDL mac hidapi 把 data[0] 當 reportID 送 7 bytes，G29 實測 STALL，**此路線在 macOS 尚無成功案例**） | high |
| **sdl2-compat 2.32.72**（Homebrew `sdl2` 只是它的 alias；`/opt/homebrew/lib/libSDL2*.dylib` 都是 symlink） | SDL2 haptic API → SDL3 (`HapticOpenFromJoystick→OpenHapticFromJoystick` 等) | 跟隨 SDL3 | 同上（SDL3 patch 即可覆蓋） | high |
| **Wine ≥ 7.0 / CrossOver / Whisky / GPTK — dinput** (`dlls/dinput/joystick_hid.c`) | 只認 HID **PID** report（usage page 0x0F）；`DIDC_FORCEFEEDBACK` 只在有 PID Device Control collection 時設 | — | 沒有 PID 裝置 → dinput 看不到 FFB。兩條可行路：(a) 遊戲資料夾放 **dinput8.dll proxy**（宣告 `DIDC_FORCEFEEDBACK`、接受 GUID_ConstantForce/Spring/Damper/…，轉發到 daemon socket；bottle 設 `dinput8=native,builtin`）— CrossFFB (MIT) 已有可用實作，PID 表擴到 c266/c267；(b) `bus_sdl` 路徑（下一列） | high |
| **Wine winebus `bus_sdl.c`** | SDL2-only（`SONAME_LIBSDL2`, dlopen `libSDL2-2.0.0.dylib`）；`SDL_JoystickIsHaptic` + `SDL_HapticQuery` 合成 PID descriptor；wheel type 時強制宣告全部 10 種 PID effect；方向用 `SDL_HAPTIC_SPHERICAL`；上游 `Enable SDL` 預設 1（wine-7.7 若 SDL 初始化成功則**取代** iohid backend） | CrossOver/Whisky 自帶 x86_64 SDL2（真 SDL2 → darwin haptic → 死）；GPTK 由 crossover-sources 22.1.1 + Homebrew sdl2（=sdl2-compat）建置 → 走 SDL3 lg4ff（若 patch）；CrossOver 25/26 bundle 的 SDL2 版本 unconfirmed | 提供 **SDL2 haptic ABI**：不是 patch SDL3 就是 libSDL2 shim | high（機制）/ medium（CrossOver bundle） |
| **Wine winebus `bus_iohid.c`** | 純 pass-through（`IOHIDDeviceSetReport` output/feature），無 haptics；只收 primary usage Generic Desktop Joystick/Gamepad；Logitech 不在 `prefer_hidraw` 清單，需 `EnableHidraw "046D:C266"` 或 `Enable SDL=0 + DisableInput=1` 才會用它 | — | 無法提供（需虛擬 PID 裝置 → entitlement） | high |
| **GameController.framework `GCRacingWheel`**（macOS 13+，SDK 26 未變） | 只有輸入（`wheelInput`：wheel/pedals/shifter、`maximumDegreesOfRotation` 唯讀）；`acquireDeviceWithError:` exclusive；`haptics` 只在 `GCController` | 是否把 c266 當 racing wheel 列出：unconfirmed（Apple 只點名 G920/G29） | 無 API 可接；只需確保 daemon 不阻擋其枚舉，並處理 exclusive acquire 導致 SetReport 失敗 | high |
| **Feral 移植（GRID Autosport、DiRT Rally/4、F1 2016/2017）** | 遊戲內嵌 wheel 碼（官方支援 DFGT/G27/MOMO/G920-900°；G29 論壇回報可用）；機制未公開 | 可能自己開 IOHID 送 `F8` 指令；是否認 c266：unconfirmed | 無法介入；daemon 應在偵測到這些遊戲時暫停 engine 避免衝突（可選） | medium |
| **原生 ETS2 / ATS（SCS）** | Mac 版無 FFB 實作（設定存在但無效） | — | **SCS telemetry plugin**（`.dylib`，x86_64 或 arm64 視遊戲）從 shared memory 讀遙測 → daemon socket → 合成 spring/damper/constant（fffb、ets2-g29-ffb-macos 模式） | medium |
| **Godot 4 / Unity / Unreal** | Godot：GCController + CHHapticEngine；Unity/Unreal 無 Mac wheel FFB | — | 無 | medium |
| **Torqer / CrossWheel（商業）** | 未公開；bottle 內放檔案 + 原生 daemon 直接對方向盤 | 證明「daemon + bottle-side DLL」在 macOS 26 + SIP on 可行；Torqer 列 G923 PS 為 beta | 參考定位，不依賴 | medium |

Wine 相關細節（實作 dinput8 proxy 時需要）：
- dinput GUID → PID usage：`GUID_ConstantForce→ET_CONSTANT_FORCE, RampForce→ET_RAMP, Square→ET_SQUARE, Sine→ET_SINE, Triangle→ET_TRIANGLE, SawtoothUp/Down, Spring→ET_SPRING, Damper→ET_DAMPER, Inertia→ET_INERTIA, Friction→ET_FRICTION, CustomForce→ET_CUSTOM_FORCE_DATA`。
- `DIEFFECT` 與 macOS `FFEFFECT` 二進位相同（時間單位 µs，強度 ±10000，`dwGain` 0..10000）；proxy 可直接把 DIEFFECT 序列化給 daemon。
- Wine 的 `bus_sdl` 會把 wheel 的軸 usage 改成 Simulation page（0xC4/0xC5/0xC6），舊模擬器可能認不到軸（g29-mac 回報）。dinput8 proxy 路線沒有此問題。

---

## 6. 建議架構與實作計畫

### 6.1 元件

| 元件 | 語言 / 形式 | 職責 |
|---|---|---|
| `G923Kit`（library） | Swift package（或 C 核心 + Swift wrapper，方便 SDL/Wine 端重用） | (1) `LG4FFCommand` encoder：第 3.8 節所有指令的 byte-exact 產生器；(2) `WheelReport` parser：12-byte input；(3) `EffectEngine`：port SDL3 `SDL_hidapihaptic_lg4ff.c`（constant/periodic/ramp → slot 0；spring/damper/friction → slot 1–3；envelope、gain、duration、iterations、2 ms tick）；(4) `HIDTransport` protocol（`open/close/setReport/inputReports/maxOutputReportSize`）與 `IOKitHIDTransport` 實作；(5) `FakeTransport` 供測試 |
| `g923ctl`（CLI） | Swift | `detect`、`switch`、`range N`、`led MASK`、`autocenter PCT`、`constant LEVEL`、`spring K`、`stop`、`init`、`monitor`（印 input）、`raw HEX`（送任意 7 bytes）、`selftest` |
| `g923d`（daemon） | Swift, LaunchAgent (`~/Library/LaunchAgents`) | 熱插拔監聽、自動 switch、套用使用者設定（range/autocenter/LED 模式）、跑 `EffectEngine`、提供 local API（UNIX domain socket + TCP `127.0.0.1:54321` 相容 CrossFFB proxy 的 wire format）、GUI/menubar 可選 |
| SDL3 patch | C（diff against libsdl-org/SDL main） | 加 c266/c267 到 `SDL_hidapi_lg4ff.c` 與 `SDL_hidapihaptic_lg4ff.c`；c267 switch case；LED；上游 PR |
| `dinput8.dll` proxy + bridge | C++（x86_64 Windows DLL，用 MinGW/clang-cl 交叉編譯；fork CrossFFB MIT） | 在 bottle 內模擬 DirectInput FFB，轉發到 `g923d` |
| SCS telemetry plugin（可選） | C（`scssdk`） | 原生 ETS2/ATS 的力回饋合成 |
| 測試 | XCTest + golden byte fixtures | 見 6.3 |

### 6.2 實作步驟（依序；每步都有「無方向盤」與「有方向盤」的驗收）

**Step 0 — 專案骨架**
- `swift package init`，targets：`G923Kit`, `g923ctl`, `g923d`, `G923KitTests`；`sdl-patch/`、`wine-bridge/`、`scs-plugin/` 子目錄；`docs/`。
- 無方向盤驗收：`swift build` 通過；CI 跑測試。

**Step 1 — Protocol encoder + parser（純函式）**
- 實作 3.6–3.9 全部指令；`TRANSLATE_FORCE`、`SCALE_COEFF`、autocenter 公式；input report parser（含 pedal 反向、hat、25 按鍵、byte 11 狀態）。
- 無方向盤驗收：golden tests 對照 new-lg4ff / SDL3 / Logitech PDF 的位元組（例：range 900 → `F8 81 84 03 00 00 00`；level 0 → `11 00 80 …`；level 32767 → `11 00 FF …`；autocenter 0xFFFF → `FE 0D 07 07 FF 00 00`＊；mode switch → `30 F8 09 07 01 01 00 00`）。＊由公式自行算出後固定為 fixture。

**Step 2 — HID transport + discovery**
- `IOHIDManager` 以 `kIOHIDVendorIDKey=0x046d`、`kIOHIDProductIDKey∈{0xc266,0xc267}` 匹配；對每個 `IOHIDDeviceRef` 讀 `kIOHIDDeviceUsagePairsKey`、`kIOHIDPrimaryUsagePageKey/UsageKey`、`kIOHIDMaxOutputReportSizeKey`、`kIOHIDMaxInputReportSizeKey`、`kIOHIDLocationIDKey`、`kIOHIDReportDescriptorKey`；選 IF0（c266：primary 0x01/0x04 且 MaxOutput=16；c267：usage pairs 含 (0x01,0x04) 且 MaxOutput=31）；`IOHIDDeviceOpen(kIOHIDOptionsTypeNone)`；`setReport` 自動補零到 MaxOutputReportSize（c266）或送 8 bytes reportID 0x30（c267）。
- 處理 `kIOReturnNotPermitted` → 提示 Input Monitoring；`kIOReturnExclusiveAccess` → 提示有其他程式 seize。
- 無方向盤驗收：`FakeTransport` 回放 c266/c267 descriptor 與 input stream（ZRtm issue #2 的 dump）；discovery 邏輯以 descriptor bytes 做單元測試；用任何一個 HID 裝置（鍵盤/滑鼠/gamepad）驗證 `IOHIDManager` 匹配、TCC 提示流程與 `setReport` 錯誤碼處理（不送實際指令）。
- 有方向盤驗收：`ioreg -p IOUSB -l -w0` / `ioreg -l -w0 | grep -B5 -A40 'C26[67]'` 記錄真實 usage pairs、report sizes、interface 數（**填補 c266 descriptor 的公開空缺**）。

**Step 3 — `g923ctl` 與 mode switch**
- `detect`：列出裝置與模式；`switch`：送 0x30 報告，等待 c267 消失 / c266 出現（IOHIDManager 的 removal/matching callback），timeout 15 s，之後延遲 2–8 s；`init`：F5 → range → 0D 00 → 11 00 80 → 23/43/83。
- 無方向盤驗收：以 FakeTransport 模擬「送出 switch → 移除 → 新增 c266」狀態機。
- 有方向盤驗收：(1) 插上 → `detect` 看到 c267；(2) `switch` → `system_profiler SPUSBDataType` 看到 c266、bcdDevice 0x3800；(3) `range 270` 後轉動到底確認鎖點；(4) `led 0x1F` 全亮；(5) `autocenter 50` 手感回中；(6) `constant 64`/`constant -64` 方向與強度；(7) 測 7 / 8 / 16 bytes 各自是否 STALL（記錄）；(8) 拔插後是否回 c267（是 → daemon 需自動重切）；(9) `F8 0A 00` 後拔插是否仍 c266。

**Step 4 — Effect engine**
- 移植 SDL3 `SDL_hidapihaptic_lg4ff.c`（zlib）到 `G923Kit.EffectEngine`：API 以 DirectInput/FFEFFECT 為模型（type、duration µs、delay、gain、direction、envelope、type-specific params；`upload/update/start(iterations)/stop/destroy/gain/autocenter/pause/resume/stopAll`），輸出為每 tick 的 slot 指令序列。tick 2 ms（`DispatchSourceTimer` leeway 100 µs，或 real-time thread）。
- 無方向盤驗收：以固定時間軸驅動 engine，斷言送出的指令序列（例：constant 50% 持續 100 ms → 第一 tick `11 00 C0…`、之後無重送、100 ms 後 `13 00…`；sine 10 Hz 的 level 曲線；spring 係數 → `21 0B …` 位元組；四個 condition 時第四個被拒或轉 damper）。
- 有方向盤驗收：`g923ctl selftest` 依序播放 constant 左/右、sine 1 Hz、spring、damper，人工確認手感與方向（**記錄 constant 正負號對應的轉向**；mainline 註解 0 = 順時針）。

**Step 5 — `g923d` daemon**
- LaunchAgent plist（`KeepAlive`, `RunAtLoad`）；設定檔（range、autocenter、LED 模式、gain、每個遊戲 profile）；socket API（JSON lines 或簡單 binary，含 `hello/caps/upload/start/stop/gain/autocenter/range/led/pause/resume`）；同時相容 CrossFFB 的 TCP 54321/54322 協定以直接沿用其 dinput8 proxy。
- 無方向盤驗收：socket API 整合測試（client → daemon → FakeTransport 指令序列）；LaunchAgent 載入/卸載；TCC 授權流程（daemon 是獨立 binary，需在 System Settings 給 Input Monitoring）。
- 有方向盤驗收：冷插拔自動切換；睡眠喚醒；與 `GCRacingWheel` 的原生遊戲並存測試。

**Step 6 — SDL3 patch**
- 修改 `supported_device_ids`（兩檔）、`IsSupportedDevice`、`SwitchMode`（c267：`{0x30, F8 09 07 01 01 00 00}` 8 bytes；hidapi mac 會把 data[0] 當 reportID）、`IdentifyWheel`（G923 無 bcdDevice 判斷；PID 即身分）、按鍵數 25、LED（G923 加入 `SetJoystickLED` gate）、`GetDeviceName`。
- 無方向盤驗收：SDL 的 `testhaptic`/`testcontroller` 用 `SDL_HIDAPI` mock？（SDL 無內建 mock）→ 至少編譯、單元測試 lg4ff 邏輯與現有 G29 相同；在 daemon 端加一個「SDL 相容 hidapi 假裝置」不切實際 → 此步以 code review + 與 G29 行為對比為主。
- 有方向盤驗收：`SDL_JOYSTICK_HIDAPI_LG4FF=1 ./testhaptic`；確認 7-byte `hid_write` 是否 STALL（若 STALL，patch 改成補到 16 bytes：SDL mac hidapi 的 `set_report` 用 `IOHIDDeviceSetReport(dev, type, data[0], data, len)`，需在 lg4ff 層補零）。成功後開 upstream PR。

**Step 7 — Wine / CrossOver 橋接**
- Fork CrossFFB：dinput8 proxy 加入 VID/PID 表 c266/c267；bridge 端改為連 `g923d`；文件化 bottle 設定 `dinput8=native,builtin`、`Enable SDL` 保持預設；提供 `.dmg` 安裝腳本。
- 無方向盤驗收：在 CrossOver/Whisky bottle 內用 Wine 的 `dinput` 測試程式或 `testdinput` 類工具列出 FFB 裝置並 `Download/Start` effect，看 daemon 收到正確指令（FakeTransport 模式）。
- 有方向盤驗收：ETS2（Windows 版）、Assetto Corsa、Live for Speed 各測一款；比對 Torqer/CrossWheel 手感。

**Step 8 — 可選：SCS telemetry plugin、menubar UI、TrueForce 合成（v2）**

### 6.3 無方向盤的測試清單（彙整）

1. Encoder golden tests（每個指令、邊界值、負值 clamp）。
2. Input parser tests（用 ZRtm issue #2 的 c267 dump、及自製 c266 12-byte 向量）。
3. Descriptor-driven discovery tests（c266/c267 descriptor bytes → 選對 interface、算對 MaxOutputReportSize、決定 reportID/長度）。
4. Effect engine 時間軸測試（constant/sine/square/triangle/sawtooth/ramp/spring/damper/friction、envelope、iterations、pause/resume、gain、stop-all、slot 溢位）。
5. Mode-switch 狀態機測試（含 timeout、重複 c267、切換失敗回退）。
6. Daemon socket API 測試（多 client、client 斷線時 stop-all、CrossFFB 協定相容）。
7. LaunchAgent 安裝/移除測試。
8. TCC/權限流程測試（用任意 HID 裝置驗證 `IOHIDDeviceOpen`/`setReport` 錯誤碼分支；不要對鍵盤送 output report）。
9. SDL3 patch：build + 與 G29 路徑的 diff review；`SDL_HINT_JOYSTICK_HIDAPI_LG4FF` 切換測試。
10. dinput8 proxy：在 bottle 內以 FakeTransport 後端跑 Wine dinput 測試程式，斷言 daemon 收到的 effect 序列。

### 6.4 有方向盤的驗證清單（第一次拿到硬體時依序做，全部記錄成 `docs/hardware-notes.md`）

1. 原始枚舉：`ioreg`、`system_profiler`、每個 IOHIDDevice 的 usage pairs / report sizes / descriptor（c267 與 c266 各一份）。
2. Mode switch 成功與時序（多久重新枚舉、校正掃描多久）。
3. Output report 長度容忍度（7/8/16 bytes）與 reportID。
4. Range、LED、autocenter、constant 各指令行為與方向正負。
5. Friction 0x0E 是否有反應。
6. 拔插 / 睡眠後是否回 c267；`F8 0A 00` 效果。
7. 與 G HUB 同時存在時是否有干擾（G HUB 開/關）。
8. `GCRacingWheel` 是否列出；原生遊戲 acquire 後 daemon 是否還能 setReport。
9. SDL3 patch 的 7-byte write 行為。
10. Wine 橋接的實際手感、延遲、是否有 500 Hz 抖動（同時兩個 writer 的徵兆）。

---

## 7. 風險與未知

### 7.1 需要實體方向盤才能確認的事項

| # | 未知 | 影響 | 目前最佳假設 |
|---|---|---|---|
| U1 | c266 模式的完整 USB/HID descriptor（IF1/IF2 內容、endpoint 編號、MaxOutputReportSize） | discovery 邏輯、補零長度 | 3 interfaces；IF0 output 16 B；endpoint 不重要 |
| U2 | macOS `IOHIDDeviceSetReport` 對 c266 的長度容忍（7/8/16）與走 interrupt 或 control pipe | 是否 STALL | 補到 16 bytes 最安全 |
| U3 | macOS 如何呈現 c267 IF0 的三個 collection（一個或三個 IOHIDDevice）、report 0x30 能否經 `IOHIDDeviceSetReport(…, 0x30, …)` 送達 | mode switch 能否從 user space 做到 | 可以（FreeTheWheel/ fffb 前例 + IOHIDLibUserClient 不驗證） |
| U4 | 切換後重新枚舉與校正時間 | daemon 時序 | 2–8 s |
| U5 | 是否每次插拔都回 c267；`F8 0A 00` 是否讓 c266 持久 | daemon 需自動重切 | 每次都回 c267 |
| U6 | 韌體是否支援 friction 0x0E | effect 對映 | 不支援 → 轉 damper |
| U7 | Constant force 正負號對應方向（文件：0 = 順時針） | 手感正確性 | 依 Logitech 文件 |
| U8 | `GCRacingWheel` 是否列出 c266，exclusive acquire 是否讓 daemon 失效 | 原生 GameController 遊戲共存 | 可能衝突 |
| U9 | SDL3 lg4ff 在 macOS 的 7-byte `hid_write` 是否可用（G29 實測 STALL） | SDL3 patch 是否還要改 mac hidapi 路徑 | 需補零 |
| U10 | G HUB dext 是否在實務上干擾（Torqer 建議移除） | 使用者指引 | 本機證據：不 match 方向盤 |
| U11 | byte 11 各狀態 bit 的確切意義 | 僅診斷用途 | — |
| U12 | Logitech 文件 `30 F8 09 05 01` 是否對某些韌體版本有效 | 無（用 0x07） | 文件錯誤 |
| U13 | TrueForce：`cur` 鏡射的正負號與縮放、串流是否影響 autocenter | v2 才需要 | 取負號（mescon） |

### 7.2 設計風險

- **裝置仲裁**：daemon、SDL3 lg4ff、Feral 遊戲、GameController 都可能同時開啟 IF0；多個 writer 會互相覆蓋（TrueForce 串流甚至會出現 500 Hz 抖動）。需明確的「誰是 writer」政策與 daemon pause API。
- **Wine 端多樣性**：CrossOver/Whisky/GPTK 各自 bundle 不同 SDL2；dinput8 proxy 是最不依賴 bundle 的方案，但遊戲自帶 dinput8 或用 XInput 時無效。
- **TCC**：`g923d` 需要 Input Monitoring；未授權時所有 `IOHIDDeviceOpen` 靜默失敗。
- **hardened runtime**：任何注入遊戲 process 的 dylib/shim 都會撞 library validation（僅 SDL3 patch upstream 與 dinput8 proxy 在 Wine 內可迴避）。
- **上游採納**：SDL3 patch 若無法在 macOS 上被驗證（維護者無硬體），可能長期停在 fork。
- **G HUB 更新**：未來 G HUB 若新增方向盤 dext，可能 seize 裝置。
- **法律/授權**：new-lg4ff 是 GPL-2.0（只當文件參考，不複製程式碼）；SDL3 zlib、CrossFFB/g29-mac MIT 可直接重用；fffb 無授權（不複製）。

### 7.3 明確不做（v1）

- ForceFeedback.framework plug-in、kext、dext。
- HID++（IF1）任何存取。
- TrueForce 串流（IF2）。
- Xbox 版 G923（c26d/c26e）。
- 虛擬 HID PID 裝置。

---

## 8. 參考來源

### 8.1 官方 / 一級來源
- Logitech Force Feedback Protocol V1.6（2020-12-17）：https://opensource.logitech.com/wiki/force_feedback/Logitech_Force_Feedback_Protocol_V1.6.pdf（本地：`scratchpad/research/protocol_ps/doc/Logitech_FF_Protocol.pdf`）
- Logitech G923 規格頁：https://support.logi.com/hc/en-us/articles/17207072404247
- Logitech G HUB 說明：https://support.logi.com/hc/en-us/articles/360023368993 ；release notes https://support.logi.com/hc/en-us/articles/360048967733
- Apple SDK headers（本機 Xcode 26.6）：`ForceFeedback.framework/Headers/{ForceFeedback.h, ForceFeedbackConstants.h, IOForceFeedbackLib.h}`；`GameController.framework/Headers/{GCRacingWheel.h, GCRacingWheelInput.h, GCSteeringWheelElement.h, GCDevice.h, GCController.h}`；`IOKit/hid/{IOHIDDevice.h, IOHIDKeys.h}`；`DriverKit.sdk/…/HIDDriverKit.framework/Headers/IOUserUSBHostHIDDevice.iig`
- 本機 `/System/Library/Frameworks/ForceFeedback.framework` v1.0.6 反組譯（`DoesServiceHaveUUID`、`FFIsForceFeedback`、`FFCreateDevice`、`FFReleaseDevice`、`FFDeviceSetForceFeedbackProperty`）
- Apple DriverKit 文件：https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development 、https://developer.apple.com/documentation/driverkit/debugging-and-testing-system-extensions 、https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit
- Apple open source：apple-oss-distributions/IOKitUser `IOCFPlugIn.c`；IOHIDFamily `IOHIDLibUserClient.cpp`, `AppleUserHIDDevice.cpp`, `IOHIDFamilyPrivate.cpp`, `IOHIDProviderPropertyMerger.cpp`, `IOHIDResourceUserClient.cpp`；xnu `IOUserClient.cpp`, `IORegistryEntry.cpp`, `IOService.cpp`, `IOUserServer.cpp`
- Linux mainline：`drivers/hid/hid-lg4ff.c`, `hid-lg.c`, `hid-ids.h`, `hid-logitech-hidpp.c`, `hid-quirks.c`, `usbhid/hid-core.c`, `hid-core.c`；commit e8ab7a10edc7（G923 Xbox）

### 8.2 開源專案（本地 clone 路徑見各報告；主要）
- berarma/new-lg4ff（GPL-2.0）— G923 PS 唯一的 Linux 驅動；PR #50 https://github.com/berarma/new-lg4ff/pull/50 ；issue #44（c267 lsusb）https://github.com/berarma/new-lg4ff/issues/44
- ZRtmWrJqXcjbqBLIMBYMCeUw/Logitech-G923-Linux-Kernel-Driver issues #1/#2/#3（descriptor dump、dmesg、失敗的 HID++ 嘗試）
- libsdl-org/SDL（zlib）— `src/haptic/darwin/SDL_syshaptic.c`, `src/joystick/hidapi/SDL_hidapi_lg4ff.c`, `src/haptic/hidapi/SDL_hidapihaptic_lg4ff.c`, `src/joystick/SDL_joystick.c`, `src/hidapi/mac/hid.c`；PR #11598
- libsdl-org/sdl2-compat — `src/sdl3_syms.h`；issue #306
- wine-mirror/wine — `dlls/winebus.sys/{bus_iohid.c, bus_sdl.c, main.c, hid.c}`, `dlls/dinput/joystick_hid.c`, `configure.ac`；wine-7.0 ANNOUNCE；commit adfee25b45（移除 joystick_osx.c）
- apple/homebrew-apple `Formula/game-porting-toolkit.rb`
- mescon/logitech-trueforce-linux-driver — `docs/TRUEFORCE_PROTOCOL.md`, `docs/PROTOCOL_SPECIFICATION.md`, `docs/FEATURE_MATRIX.md`, `docs/SDK_ABI_NOTES.md`, `sdk/README.md`, `mainline/hid-logitech-hidpp.c`, `userspace/logi-wheel/crates/logi-tf-sim/src/g923.rs`
- Mhytee/Trueforce-For-All — `src/TrueforceForAll.Core/{UsbPcapFfbTap.cs, WheelDiscovery.cs, TrueforceDevice.cs, InitData.cs}`
- sonik-br/lgff_wheel_adapter — `usb_descriptors.h`, `reports.h`, `wheel_commands.h`
- botsofcog/LogiWheelHost — `src/LogiWheelHost.h`（c266 按鍵 mask 實測）
- hm0429/logitech-g923（node-hid）；matlo/GIMX `ff_lg.h/.c`, `g29_ps4.h`；PCSX2 `lg_ff.cpp`
- macOS 前例：karrvel/g29-mac（MIT）、teosemi/CrossFFB（MIT）、AndreyZarembo/ets2-g29-ffb-macos、eddieavd/fffb（無授權）、CesarOvilla/g923-mac-ffb（Xbox 版）、jackhumbert/FreeTheWheel（Feral, GPL）、360Controller（Feedback360/XBOBTFF, GPL）、hjelmn/gcusbadapter_osx
- 商業：https://torqer.app/ 、https://crosswheel.seastian.com/

### 8.3 論壇 / 二手來源（低至中信心）
- MacRumors threads 2370043、2347162、1658469、2047822；Apple Community 255432054、251997387；SCS forum t=334284；Steam 討論 255220 / 227300 / 270880；tekbyte.net（G920 on macOS 12）；CodeWeavers forum msg=251468 / 314698（僅搜尋摘要）；lfsmanual.net；businesswire TrueForce 新聞稿；ublue-os/bazzite issue #4306（Xbox 版）

### 8.4 本地研究目錄
- `/private/tmp/claude-501/-Users-chung-g923/eaeb0323-fb57-41e8-aa16-a5fcd7f4ef6b/scratchpad/research/{protocol_ps, hidpp_trueforce, macos_ff_framework, macos_registration, existing_projects, consumers, verify_*}/`

---

## 9. 多代理人研究的補充與校正（2026-09-08 併入）

一組 13 個代理人的獨立研究（含對抗式驗證）完成後，補充了以下細節，並與本報告核對。**兩處與本報告先前結論不同的地方，以本機實測為準。**

### 9.1 關於「ForceFeedback 外掛在 SIP 下不可行」的校正

該研究的兩個反組譯驗證者，因為 `DoesServiceHaveUUID` 寫死 `/System/Library/Extensions/`，推論外掛路線在 SIP 下「不可行」。**這個結論是靜態分析的侷限，本機實測推翻它**：

- 我用 `../` 逃逸（把 `IOCFPlugInTypes` 值設為 `../../../<家目錄路徑>/G923FF.plugin`），讓 `stat("/System/Library/Extensions/../../../…")` 與後續 `dlopen` **都成功**，外掛從家目錄載入。
- `make e2e` 讓真正的 `ForceFeedback.framework` 走完 `FFIsForceFeedback → FF_OK`、`FFCreateDevice → FF_OK`，且回報的能力遮罩 `0x7ff` 正是本外掛的簽名值——證明框架確實呼叫了我們的 vtable。

因此外掛路線是**實測可行**，不需關 SIP、不需 kext。研究提出的判別碼 `0xE00002BE / FFERR_INVALIDPARAM 0x80000003` 正是「未逃逸、`stat` 失敗」時的回傳，與我未逃逸時看到的現象一致。

### 9.2 但要補一個重要架構觀點：不是所有遊戲都走 ForceFeedback.framework

研究正確指出：`ForceFeedback.framework` 外掛只服務**會呼叫它的**軟體。分佈情況：

- **SDL2 的 haptic（`src/haptic/darwin`）走 ForceFeedback.framework** → 我們的外掛直接生效。
- **SDL3 改用自己的 hidapi lg4ff 驅動**（不走框架）→ 需另外把 G923 的 PID 加進 SDL3 的 `SDL_hidapi_lg4ff.c`（上游 PR）。
- **Wine/CrossOver/GPTK** 的 DirectInput FFB 依後端而定；可用 dinput8 代理（fork `teosemi/CrossFFB`）轉發到常駐程式。

結論：**外掛是「涵蓋最廣的通用路線」且已實測可行**；SDL3 patch 與 dinput8 代理是**互補**的擴充，用來涵蓋不走框架的遊戲。建議之後把 `g923d` 加上一個本機 socket API，讓這些橋接共用同一顆效果引擎。

### 9.3 已依研究校正的程式（本次已改）

- **輸出報告補零到裝置的 `kIOHIDMaxOutputReportSize`（G923 c266 預期 16 bytes）**。原本只送 7 bytes，真機可能忽略。已改為查詢並補零。
- **介面選擇**：方向盤有多個 HID 介面（IF0 搖桿 `usage 0x01/0x04`、IF1 HID++、IF2 TrueForce）。已改為優先選 primary usage 為搖桿且有輸出報告的節點，**絕不碰 IF1/IF2**。
- **初始化序列**加上 fixed-time-loop 關閉（`0D 00`）與清空所有 slot（`11 00 80` + `23`/`43`/`83`），對齊 new-lg4ff / G HUB。

### 9.4 已驗證的 c266 native 模式輸入報告版面（12 bytes，無 report id）

供之後自寫輸入解析或除錯用（macOS 本來就能讀，通常不必自己解）：

- byte0：bit0-3 方向帽（0=N…7=NW,8=放開），bit4-7 = Cross/Square/Circle/Triangle。
- byte1：R1/L1 撥片、R2/L2、Share、Options、R3/L3。
- byte2：bit0-5 H 排檔 1-6，bit6 倒檔，bit7 「+」。
- byte3：bit0「−」、bit1/2 旋鈕 CW/CCW、bit3 旋鈕按下、bit4 PS 鍵。
- **byte4-5：方向盤 16-bit 小端序**（0=最左，約 0x8000 置中，65534=最右）。
- byte6 油門（Z）、byte7 煞車（Rz）、byte8 離合（Y，反向：0xFF=放開）。
- byte9/10 排檔桿 X/Y，byte11 狀態位元（bit1 踏板未接、bit2 供電/校正）。
- 來源：LogiWheelHost、lgff_wheel_adapter、SDL `SDL_hidapi_lg4ff.c`。c266 高信心。

### 9.5 USB ID 細節校正

- c266（native/PC）與 c267（PS）**bcdDevice 都是 0x3800**，所以只能用 PID 區分，不能用版本號。
- G923 **不使用** Driving Force 的 `F8 0A / F8 09 xx 01` 開機切換機制（那是 G25/27/29）；G923 PS→native 用 report id `0x30` + `F8 09 07 01 01`。
- 實作者回報只有 `DEVICE=0x07`（G923）這個切換值有效，文件上的 `05` 不一定行。

### 9.6 研究列出、需實機確認的項目（補充第 7 節）

- c266 IF0 的 `kIOHIDMaxOutputReportSize` 實際值（預期 16）與 macOS 對 7/8/16-byte 輸出報告長度的容忍度。
- 模式切換後重新列舉與校正時間（預期 2-8 秒，個別 Linux 主機曾見約 1 分鐘卡頓）。
- **摩擦力 type `0x0E` 是否被 G923 韌體實作**（Logitech 文件只列 DFP/G25/DFGT/G27；new-lg4ff 對 G923 把摩擦/慣性映射成阻尼）。本外掛目前仍宣告 friction，若實機無效應改為映射到 damper。
- 常力方向與手感強度對應（Logitech 文件：`0x00` = 順時針）；自動回正強度對應。
- `GameController.framework` 是否把 c266 列為 `GCRacingWheel`，以及原生遊戲的獨佔開啟是否會讓常駐程式的 `setReport` 失敗。
- TrueForce（IF2，vendor `0xFFFD/0xFD01`，64-byte 串流）技術上可自行合成，但會覆蓋 classic FFB 且非遊戲原生訊號，列為 v2 之外。

---

## 10. TrueForce v2 骨架與歐卡（ETS2）路徑（2026-09-08 加入）

### 10.1 已加入 repo 的 v2 元件（預設不啟用）

- **`src/tools/g923_probe_if2.c`**：實機探測工具。列出方向盤所有 HID 介面的 usage page/usage、輸入/輸出報告大小，標示哪個是 IF0 搖桿、IF1 HID++、IF2 TrueForce；`--dump-desc` 印出各介面的 HID report descriptor 原始位元組；`--silence N` 送靜音封包測試 IF2 是否接受（實驗、需實機）。
- **`src/common/g923_trueforce.{h,c}`**：TrueForce 串流模組骨架。開啟 IF2（usage `0xFFFD/0xFD01`）、封包編碼（report id `0x01` + 13 樣本滾動視窗、每包 4 個新 16-bit 樣本、`0x8000`=靜音）、串流迴圈，以及兩個範例訊號源：`mirror`（把 classic 力鏡射成低頻振動）與 `telemetry`（引擎轉速 + 路面顆粒感，供 ETS2/ATS 用）。**封包格式與 init 握手皆標示 UNVERIFIED**，需用探測工具在實機確認後才可信。
- 純編碼部分（`g923_tf_encode_packet` 的滾動視窗）已納入單元測試（`make test` 現為 47 項）。

### 10.2 歐卡（Euro Truck Simulator 2，Steam）建議路徑

使用者主要目標是 ETS2。兩條路，建議如下:

1. **相容層跑 Windows 版（最穩）**：CrossOver / Whisky / Wine 帶 `disable-library-validation`,一定載入我們的 `G923FF.plugin`,經典力回饋（方向盤重量、路感、回正）可用。
2. **原生 macOS 版**：走 SDL;SDL2 haptic 在 macOS 經 `ForceFeedback.framework`,理論上外掛可服務,但需實機確認該版本是否以強化執行期 + 函式庫驗證擋外掛。

**TrueForce 對歐卡的最佳做法**是「自製振動層」而非移植：ETS2/ATS 有官方 **SCS 遙測 SDK**(跨平台,plugin 放進遊戲的 `plugins/` 目錄,以共享記憶體提供引擎轉速、車速、輪胎狀態)。把這些餵進 `g923_tf_source_telemetry`,即可在 IF2 上做出隨引擎/路面變化的高頻振動。這條 v2 工作項需:(a) 用 `g923_probe_if2` 確認 IF2 封包格式,(b) 寫 SCS 遙測讀取器接上 `g923_trueforce`。

### 10.3 SCS 遙測（ETS2/ATS）— 已加入骨架

已把「自製 TrueForce 訊號源」的遙測鏈路寫好骨架：

- **共享記憶體契約** `src/common/g923_telemetry_shm.h`：外掛與讀取器之間的 struct，用 seqlock 讓讀取一致、不擋寫入。
- **讀取器** `src/common/g923_telemetry_reader.{h,c}`：開啟/取樣共享記憶體、從懸吊變化推導「路面粗糙度」、並轉成 `g923_tf_telemetry_ctx`（TrueForce 訊號源吃的格式）。這部分是純我們的程式，已 build + 單元測試（`make test` 現含 SCS 遙測往返測試 9 項）。
- **檢視工具** `src/tools/g923_telemetry_dump.c`：不需方向盤即可確認外掛有在送資料（`--watch` 即時刷新）。
- **SCS 外掛** `src/scs-plugin/g923_scs_plugin.c`：由遊戲載入、訂閱轉速/上限/車速/檔位/懸吊等頻道、寫進共享記憶體。它需要官方 SCS SDK 標頭,所以不在預設 `make`,改用 `make scs-plugin SCS_SDK=/path`（見 `src/scs-plugin/README.md`）。已用代表性的 SDK 型別做過語法檢查。

### 10.4 v2 待辦（實機後）

1. `g923_probe_if2 --dump-desc` 抓 c266 模式下 IF0/IF1/IF2 完整 descriptor,補上第 9.4/9.6 節的未知。
2. 確認 IF2 封包格式(封包長度、樣本數、取樣率、init 握手),更新 `g923_trueforce.c` 中所有標 UNVERIFIED 的常數。
3. 用官方 SCS SDK 編 `make scs-plugin`,裝進歐卡,用 `g923_telemetry_dump` 確認遙測進來。
4. 把遙測讀取器 → `g923_tf_source_telemetry` → TrueForce 串流串起來,併入 `g923d`,並與 classic FFB 做仲裁(串流時會覆蓋 classic)。
5. CrossOver/Whisky 的 Windows 版歐卡需要 Windows 版外掛(.dll),用同一份 `g923_scs_plugin.c` 在 Windows / mingw 編。
