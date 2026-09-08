# 原生 macOS 版 ETS2 + G923 力回饋：現況與務實做法

> 更新日期：2026-09-08
> 對象：Apple Silicon Mac、macOS 26 (Tahoe)、SIP on、Logitech G923
> 資料來源：SCS 官方論壇、ETS2 Steam 商店頁、scs-sdk-plugin 專案、doesitarm/AppleGamingWiki、Torqer 官網、以及本機對本專案 plugin 的實測。研究已通過交叉查證（verification）。

---

## 1. 結論（先講一句話）

**原生 macOS 版歐卡（ETS2）目前「開得起來、方向盤能轉、按鍵能對應，但沒有力回饋（no FFB）」——這點從 2020 一路到 2026 社群回報一致，SCS 從未宣布修復。所以「原生版 + G923 力回饋」目前是「未證實可行、且大機率被擋」的路；要保證有力回饋，正解是跑 Windows 版透過 CrossOver/Whisky，本專案的外掛正是在這條路上已驗證可載入。**

### 原生 vs CrossOver 現實比較

| 項目 | 原生 macOS 版（Steam .app） | Windows 版透過 CrossOver/Whisky |
|---|---|---|
| 架構 | Intel x86_64，透過 Rosetta 2 執行（**無 arm64 原生版**） | Windows x86_64，經 Wine/CrossOver 再 Rosetta |
| 繪圖 | 預設 deprecated OpenGL（慢）；社群 flag `-rdevice vk`（MoltenVK→Metal）可約略翻倍 FPS | DXVK/D3DMetal，效能普遍較佳 |
| 效能 | 較差（M1/M3/M4 上約 20–30 FPS，含熱節流）；社群普遍反而推薦 CrossOver | **較好**（社群多次回報 Windows 版經 CrossOver 比原生更順） |
| 方向盤輸入 | 可用（可能需 FreeTheWheel / 睡眠小技巧解 900 度） | 可用 |
| **力回饋 FFB** | **不會動**（settled 2022–2026） | **可行**：CrossOver + Torqer（商業 app，約 US$12）已明確支援 ETS2/ATS 的 G923；本專案外掛亦在此宿主類別已驗證可載入 |
| 本專案外掛能否服務 | 疑慮重重（見第 3、4 節） | **是**——已在 non-hardened / hardened+disable-library-validation 宿主驗證載入成功 |

> 重點反直覺事實：原生版在 macOS 上其實是「效能較差」的那條路，它唯一的結構性優勢本來應該是「原生 process 可以吃 ForceFeedback.framework」，但偏偏 SCS 的原生版根本沒把 FFB 打通。

---

## 2. SCS 對原生 macOS 的現況

- **仍在架上、未宣布停更**：ETS2 Steam 頁（app 227300）仍列 macOS 平台，最低與建議 OS 皆為 macOS 11 (Big Sur)。需求區塊仍寫 Intel 顯卡（Intel HD 630 最低 / GTX 1660 建議），**完全沒有 Apple Silicon / Rosetta 字樣**——本身就是「Mac 需求已多年未更新」的證據。
- **不是 arm64 原生**：Mac 版是 **x86_64（Intel）binary，靠 Rosetta 2 在 Apple Silicon 上翻譯執行**。沒有任何來源指出存在原生 Apple Silicon build。(doesitarm：「Yes, works via Rosetta 2」。)
- **繪圖是 deprecated OpenGL**：無原生 Metal backend。社群 flag `-rdevice vk`（可搭 `-force_modern_device`）走 Vulkan→MoltenVK→Metal，回報 FPS 約翻倍，但屬非官方且仍在 Rosetta 上。**FFB 與繪圖無關，切 Vulkan 不會讓 FFB 出現。**
- **Vulkan「進行中」、Metal 無承諾**：最強的前瞻說法來自論壇 global moderator（Madkine，2026-03-22）「SCS 一直在做 Vulkan，但不知何時公開」——**這是版主/社群志工，不是 SCS 開發者官方部落格**。沒有 Mac 專屬的 Metal 或 arm64 路線圖。信心：中。
- **ATS 與 ETS2 同引擎、同 macOS 狀態**（同 OpenGL、同 Rosetta/x86_64、同 Vulkan-in-progress）。
- **Rosetta 時程**：macOS 26（本機）Rosetta 2 完整；macOS 27（2026 秋）Apple-Silicon-only 但保留完整 Rosetta 2；macOS 28（2027 秋）只留遊戲用途的 Rosetta 子集。→ Intel-Rosetta 原生版 + x86 外掛至少可撐到 2027，但跑道在縮短。

---

## 3. 原生版力回饋走哪條路，我們的外掛能不能服務它

**關鍵未證實事實（native path 的成敗核心）：沒有任何來源證實 SCS 原生 Mac binary 到底有沒有把 wheel FFB 接到 `ForceFeedback.framework`。**

- macOS **本身有能力**對方向盤做 FFB（Feral 的 F1 2017 / Dirt Rally 在 macOS 上 FFB 正常）——所以「沒 FFB」是**遊戲端問題，不是 OS 限制**。
- 但 ETS2 原生版可能：(a) 根本沒呼叫 ForceFeedback.framework（只讀 HID/SDL 輸入軸），或 (b) 有一條 dormant/broken 的路。若是 (a)，**我們的 plugin+daemon 就算完美註冊在 IOKit node 上，也沒有任何東西會來驅動它——外掛會閒置**。
- **本機實測到的第二個硬阻礙（架構不符）**：本專案 `build/G923FF.plugin/Contents/MacOS/G923FF` 目前是 **arm64-only**（`lipo -archs` 確認）。原生 ETS2 是 **x86_64 / Rosetta process**。**ForceFeedback CFPlugIn 必須符合宿主 process 架構——arm64-only 的外掛不會被載入 Rosetta x86_64 宿主**。要有任何機會，外掛必須改為 universal（x86_64 + arm64）或至少含 x86_64 slice。Makefile 目前沒有 `-arch` 旗標，只出 host（arm64）slice；`scs-plugin` target 出的 `g923_telemetry.dylib` 同樣是 host arm64，用在原生遊戲上也有相同架構問題。

**外掛對原生版的可服務性：疑慮重重、未證實。** 兩個必過關卡：(1) ETS2 原生版是否真的呼叫 ForceFeedback.framework；(2) 外掛要有 x86_64 slice 才載得進 Rosetta 宿主。兩者目前都不成立/未知。

> 備援路線（v2）：若原生版不呼叫 FFB，唯一能在原生版產生震動效果的辦法是**用 SCS telemetry plugin（`.dylib`，放在 `.../Euro Truck Simulator 2.app/Contents/MacOS/plugins`）從遙測資料自行合成 rumble**，而不是等遊戲的 FFB 呼叫。原生版**確實支援 telemetry plugin**（truckermudgeon/scs-sdk-plugin，2025-12 維護者證實 Win/Linux/macOS 三平台皆支援）。但這條 telemetry dylib 一樣必須是 x86_64 才載得進 Rosetta 遊戲。ETS2 專屬路徑是由 ATS 路徑類推，尚未獨立文件化。

---

## 4. code signing / library validation 會不會擋我們的外掛，怎麼用 codesign 檢查

**規則（本專案已證實）**：外掛只能載入「**非 hardened runtime**」或「**hardened + `com.apple.security.cs.disable-library-validation`**」的宿主。若宿主是 hardened runtime **且**啟用 library validation（無該 entitlement），外掛被擋。

- **CrossOver/Whisky/Wine/GPTK 這類宿主帶 `disable-library-validation`→ 外掛可載入（已驗證）**。
- **原生 ETS2 的簽章狀態未知**（本機未安裝遊戲，無法讀 codesign 輸出）。但有間接證據 library validation **可能沒開**：社群的 telemetry plugin 是使用者本機自行編譯（ad-hoc / team ID 不符），卻能載入原生版——library validation 正常會擋這種 team ID 不符的 dylib。這是「大概沒開」的旁證，**不能取代直接讀 codesign flags**。
- 附帶提醒：本專案自己的 `G923FF.plugin` 目前是 **adhoc 簽章、arm64 thin、TeamIdentifier not set**（本機 `codesign -dv` 確認）。

**遊戲到手安裝後，用這三個指令定案（原生 path 的三個 gating check）：**

```bash
APP="$HOME/Library/Application Support/Steam/steamapps/common/Euro Truck Simulator 2/Euro Truck Simulator 2.app"

# (1) 架構：預期只有 x86_64（證實 Rosetta）
lipo -archs "$APP/Contents/MacOS/"* 2>/dev/null
file "$APP/Contents/MacOS/"*

# (2) 是否呼叫 ForceFeedback.framework（native path 的成敗核心）
otool -L "$APP/Contents/MacOS/"* | grep -i ForceFeedback
#   有列出 = 遊戲連結了框架（值得進一步 dtrace 追 FFDeviceCreate）
#   沒列出 = 遊戲根本沒用框架 → 我們的 ForceFeedback plugin 對原生版無效

# (3) hardened runtime + library validation
codesign -d --entitlements :- --verbose=4 "$APP" 2>&1 | sed -n '1,40p'
#   看 CodeDirectory 的 flags= 那行：
#     含 "runtime"  → 有 hardened runtime；再確認有沒有 disable-library-validation entitlement
#     不含 "runtime"→ library validation 未強制，第三方 dylib/plugin 可載入
codesign -d --entitlements :- "$APP" 2>&1 | grep -i disable-library-validation
```

判讀：
- (2) 若 **grep 不到 ForceFeedback** → 原生 path 直接判死，走 CrossOver。
- (3) 若 flags 含 `runtime` **且** 沒有 `disable-library-validation` → 外掛被擋，原生 path 判死。
- 每次 SCS 更新後 flags/entitlements 可能變，需重驗。

---

## 5. 最務實的做法與逐步步驟

### 現實判斷
原生 path 同時卡在：FFB 是否被呼叫（未知，且社群一致回報「原生沒 FFB」）、外掛架構不符（arm64-only）、簽章可能擋。**因此把「保證有力回饋」寄託在 CrossOver 路線，原生 path 當作待驗證的實驗。**

### 路線 A（推薦、保證可用）：Windows 版 + CrossOver/Whisky + 本專案外掛
1. 安裝 CrossOver（或 Whisky/GPTK）。
2. 在一個 bottle 裡裝 Steam，下載 **Windows 版** ETS2（不是 macOS 版）。
3. 確認 wheel 在系統層被看到（必要時每次插上跑一次 FreeTheWheel 解 900 度）。
4. 部署本專案：啟動 `g923d` daemon，讓它把 ForceFeedback CFPlugIn 註冊到 wheel 的 IOKit node（「../」逃逸路徑）。
5. 進遊戲，Options → Controls 選方向盤，開 Force Feedback，設 gain，開車測試路面/緣石回饋。
6. （替代/對照）也可直接買 **Torqer**（torqer.app，約 US$12，5 天試用）——它專門橋接 CrossOver/Heroic/Wine 裡 Windows 遊戲的 FFB 到 macOS 方向盤，官方明列支援 ETS2/ATS + G923。**注意 Torqer 只吃 Windows build，不支援原生 macOS 版**——可用來當「路線 A 是否成立」的快速對照。

### 路線 B（實驗、可選）：原生 macOS 版 + 本專案外掛
1. 安裝原生 macOS ETS2。
2. 跑第 4 節三個 check：`lipo -archs` / `otool -L | grep ForceFeedback` / `codesign -d --entitlements`。
3. 若 `otool` 有 ForceFeedback 且簽章不擋：把外掛 **改編為含 x86_64 slice**（universal），重新 daemon 註冊，進遊戲測 FFB。
4. 若 `otool` 沒 ForceFeedback：改走 v2 **telemetry rumble** 路線——`make scs-plugin`（需先確保產出含 x86_64 slice），把 `g923_telemetry.dylib` 放進 `Euro Truck Simulator 2.app/Contents/MacOS/plugins`，由遙測驅動震動。
5. 效能不佳時可試 `-rdevice vk`（+`-force_modern_device`）換 Vulkan 提升 FPS（與 FFB 無關）。

### 一句話建議
**先做路線 A 拿到穩定力回饋（今天就能成立），把路線 B 當研究專案；路線 B 的第一步永遠是那三個 codesign/otool/lipo 檢查。**

---

## 6. 方向盤 + 遊戲到手當天檢查清單

- [ ] G923 插上，切到對的模式（PC/PlayStation 模式開關）。
- [ ] 系統有沒有認到 wheel（`ioreg -p IOUSB | grep -i logitech` 或 系統資訊 USB）。
- [ ] 若轉不到 900 度：跑 FreeTheWheel，或插上後讓 Mac 睡一下再喚醒。
- [ ] 決定路線：先安裝路線 A（CrossOver + Windows 版）拿保證可用的 FFB。
- [ ] 若也要試原生版：安裝後**立刻**跑第 4 節三個指令並記錄輸出（架構、有無 ForceFeedback、hardened/LV flags）。
- [ ] 確認本專案外掛的架構：`lipo -archs build/G923FF.plugin/Contents/MacOS/G923FF`（目前 arm64-only，原生 x86_64 遊戲需重建為 universal）。
- [ ] 啟動 `g923d`，用 `build/ff_probe` 之類先確認 daemon 能對 wheel 下 constant/spring 效果（不經遊戲）。
- [ ] 進遊戲 Controls：選方向盤、開 FFB、設 gain、實車測回饋。
- [ ] 效能不足：試 `-rdevice vk` flag。
- [ ] 每次遊戲更新後：重跑第 4 節 codesign 檢查（flags 可能變）。

---

## 7. 尚待實機／實遊戲確認的項目

1. **[最高價值] 原生 ETS2 binary 有沒有連結/呼叫 `ForceFeedback.framework`**——`otool -L` + 必要時 dtrace 追 `FFDeviceCreate`。這決定 ForceFeedback plugin path 對原生版是否有意義。（本機未裝遊戲，尚未驗。）
2. **原生 ETS2 的 hardened runtime / library validation 實際 flags**——`codesign -d --entitlements`。決定外掛會不會被簽章擋。（尚未驗。）
3. **原生 binary 架構確認為 x86_64**（預期是），並據此把本專案外掛/telemetry dylib 重建為含 x86_64 slice。（本專案現況 arm64-only，已確認需改。）
4. **Mac binary 是否仍與 Windows 版同步更新**、最後更新日期——無法從 primary source 定案。
5. **ETS2 專屬 plugin 目錄** `Euro Truck Simulator 2.app/Contents/MacOS/plugins` 是由 ATS 路徑類推，需在實機確認。
6. **CrossOver/Whisky 對 G923 FFB 在 Apple Silicon 今天的可靠度**——社群回報混雜；Torqer 提供了商業化的穩定解，但需自行試用確認。
7. **TrueForce（v2 rumble 計畫）** 在 ETS2/ATS 的支援即使在 Windows 也尚不成熟——會影響 v2 效果品質。

---

### 附：本機已確認事實
- `build/G923FF.plugin`：Mach-O **thin arm64**、**adhoc** 簽章、TeamIdentifier not set。→ 不能載入 Rosetta x86_64 宿主，需改 universal。
- Makefile 無 `-arch` 旗標，`plugin`/`scs-plugin` 皆只出 host(arm64) slice。
- 本機**未安裝 ETS2**，故第 1–3 項的遊戲端檢查尚未執行。
