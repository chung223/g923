# g923-mac — 讓 Logitech G923 在 macOS 上有力回饋（Apple Silicon、SIP 開啟）

一套 user-space 驅動，讓 Logitech **G923**（以及 G29 家族）方向盤在現代 macOS 上**有力回饋（FFB）**，**不需要 kext、也不需要關閉 SIP**。

- 輸入（方向、踏板、按鍵）本來就能透過 macOS 內建 HID 使用。
- 這個專案補上缺的部分：**力回饋**、**轉向角度**、**LED**、**自動回正**，以及 **PlayStation→native 模式切換**。
- 它**不提供 TrueForce**（那是 Logitech 私有、只在 Windows SDK 開放的通道）。你會得到「經典力回饋」：常力、彈簧、阻尼、摩擦、週期波——對模擬賽車的路感與方向盤回正已經足夠。

完整研究、精確的協定位元組、以及 macOS 載入機制的原理，請見 [`docs/research-report.md`](docs/research-report.md)（繁體中文）。

## 運作原理（簡述）

遊戲透過 Apple 的 `ForceFeedback.framework` 溝通，框架會為每個裝置載入一個外掛。我們提供這個外掛（`G923FF.plugin`），把 DirectInput 式的效果翻成 Logitech 的經典 HID 指令，直接送到方向盤。

框架平常只在密封唯讀的 `/System/Library/Extensions/` 下找外掛。一個小型代理程式（`g923d`）會在方向盤的 IOKit 節點上，用一個以 `../` 逃逸該目錄的路徑來註冊我們的外掛，讓外掛可以放在你的家目錄。這個做法已在 **macOS 26.2 / Apple Silicon、SIP 開啟、一般使用者權限**下實測可行。

## 哪些軟體會有力回饋

| 執行遊戲的宿主 | 有 FFB 嗎？ |
|---|---|
| **CrossOver / Whisky / Wine / Game Porting Toolkit**（Windows 模擬賽車） | **有** |
| 未啟用函式庫驗證的原生 App / SDL 遊戲 | 有 |
| 強化執行期 **+ 函式庫驗證** 的原生遊戲 | 沒有（需 Developer ID 簽章 + 公證的外掛，且該遊戲仍可能拒載） |

## 建置與測試（不需方向盤）

```bash
make            # 建置外掛、常駐程式、CLI
make test       # 42 項不需硬體的單元測試（協定 + 效果數學）
make e2e        # 端對端：真正的 ForceFeedback.framework 載入真正的外掛
```

## 安裝（單一使用者、SIP 維持開啟）

```bash
make install    # 安裝到 ~/.local/g923 並載入 LaunchAgent
```

接著，把方向盤插上後：

```bash
~/.local/g923/g923ctl list          # 列出方向盤 + 是否已接好 FFB
~/.local/g923/g923ctl mode-native   # 只有顯示「PlayStation mode」時才需要
~/.local/g923/g923ctl force 20000   # 方向盤應拉力約 2 秒
~/.local/g923/g923ctl range 900     # 設定轉向角度（度）
```

移除：`make uninstall`。

## 歐卡（ETS2，Steam 版）

完整分析見 [`docs/native-macos-ets2.md`](docs/native-macos-ets2.md)。重點：

- **原生 macOS 版沒有力回饋**。社群 2020–2026 一致回報：開得起來、方向盤能轉、按鍵能對應，但 FFB 不會動，SCS 從未修。而且原生版是 **x86_64 靠 Rosetta 2** 執行（非 arm64），效能也較差。
- **要有力回饋 → 跑 Windows 版透過 CrossOver / Whisky**。這些相容層帶 `disable-library-validation`，一定載入我們的外掛。這是保證可用的路。
- 本專案外掛已改為 **universal（x86_64 + arm64）**，因為 CFPlugIn 必須符合宿主 process 架構，而重要的宿主（Rosetta 遊戲、Wine 行程）是 x86_64。

ETS2/ATS 有官方 **SCS 遙測 SDK**，是「自製 TrueForce 振動層」（見 v2）最理想的訊號來源：可用引擎轉速、輪胎打滑、路面顆粒感產生高頻振動。

## TrueForce（v2，實驗中，需實機）

TrueForce 走的是方向盤的第 3 個 HID 介面（vendor `0xFFFD/0xFD01`），與經典力回饋不同。**遊戲原生的 TrueForce 內容無法在 Mac 取得**（那需要 Windows 私有 SDK），但我們可以**自己合成**振動灌進這條通道。這部分標為 v2、預設不啟用，且封包格式尚未經實機確認。

方向盤到手後，先用探測工具確認介面與格式：

```bash
make tools
./build/g923_probe_if2 --dump-desc     # 列出方向盤所有 HID 介面 + descriptor
./build/g923_probe_if2 --silence 100   # (實驗) 送靜音封包，看 IF2 是否接受
```

## 目錄結構

```
src/common/    協定編碼、效果引擎、HID 傳輸、注入、裝置搜尋、TrueForce(v2)、遙測讀取(v2)
src/plugin/    G923FF.plugin — ForceFeedback CFPlugIn 外掛
src/daemon/    g923d — 方向盤插上時自動註冊外掛的 LaunchAgent
src/cli/       g923ctl — 控制與診斷
src/tools/     g923_probe_if2（TrueForce 介面探測）、g923_telemetry_dump（遙測檢視）
src/scs-plugin/ ETS2/ATS 的 SCS 遙測外掛骨架（需 SCS SDK，另外編）
src/test/      單元測試（協定 + 遙測）+ ff_probe（模擬遊戲）
scripts/       安裝 / 移除 / 端對端測試
docs/          研究報告（繁體中文）
```

遙測（v2，給 TrueForce 用）：把 SCS 外掛裝進歐卡（見 [`src/scs-plugin/README.md`](src/scs-plugin/README.md)），
再用 `./build/g923_telemetry_dump --watch` 檢視即時轉速/車速/路面粗糙度是否有進來。

## 目前狀態

除了「方向盤上的實際行為」以外，其餘都已在本機建置並驗證。仍需實體方向盤確認的部分：共享開啟時的輸出報告、是否跳「輸入監控」授權、模式切換重新列舉的時序、以及實際手感。詳見研究報告第 7、9.6 節。

## 授權

本專案採 **GPL-2.0**（見 [`LICENSE`](LICENSE)）。原因：其 Logitech 協定位元組是從 GPL-2.0 的 `berarma/new-lg4ff` Linux 驅動逐欄轉錄而來。

## 來源與致謝

Logitech 協定來自 GPL-2.0 的 `berarma/new-lg4ff` 與 Linux 主線 HID 驅動；macOS 內部細節來自 Apple 開源的 `IOKitUser` / `IOHIDFamily` 以及 `ForceFeedback` 框架的標頭檔與反組譯。
