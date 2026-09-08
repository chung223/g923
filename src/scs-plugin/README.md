# SCS 遙測外掛（ETS2 / ATS）— v2

這個外掛由 **Euro Truck Simulator 2 / American Truck Simulator** 載入，把遊戲狀態
（引擎轉速、上限、車速、檔位、每個輪子的懸吊變化）寫進 POSIX 共享記憶體
`/g923_telemetry`（格式見 [`../common/g923_telemetry_shm.h`](../common/g923_telemetry_shm.h)），
供 `g923d` 讀取來驅動 v2 的 TrueForce 振動層。

> 這是 v2、實驗性的元件。與經典力回饋（`G923FF.plugin`）無關，沒有它也能玩。

## 為什麼不在 `make` 裡編

外掛需要**官方 SCS SDK 標頭檔**（`scssdk_telemetry.h` 等），我們不便直接內含。
因此預設不編，改用專門的 target 並指定 SDK 路徑。

## 取得 SCS SDK

- 遊戲安裝目錄下常附一份（例如 Steam 的 ETS2 資料夾內 `sdk/`）。
- 或從 SCS 官方 modding wiki 下載遙測 SDK（`scs_sdk_x.xx.zip`），解壓後裡面有 `include/`。

## 編譯

同一份 `g923_scs_plugin.c` 兩種目標，差別只在共享資料的傳輸方式（見下）。

### A. 原生 Mac 版遊戲 → `.dylib`

```bash
make scs-plugin SCS_SDK=/path/to/scs_sdk
# 產出 build/g923_telemetry.dylib
```

### B. CrossOver / Whisky / Wine 跑的 Windows 版遊戲 → `.dll`（交叉編譯）

需要 mingw-w64：

```bash
brew install mingw-w64
make scs-plugin-win SCS_SDK=/path/to/scs_sdk
# 產出 build/g923_telemetry.dll（x86_64）
```

## 跨 Wine 邊界怎麼共享資料

- **原生版**：外掛用 POSIX 共享記憶體 `/g923_telemetry`；讀取器直接讀。
- **Windows 版（Wine）**：Windows 外掛在 Wine 裡，macOS 讀取器在外面，不能共用同一個具名記憶體。做法是**檔案支援的記憶體映射**：外掛寫 `Z:\tmp\g923_telemetry.bin`，而 Wine 預設把磁碟 `Z:` 對應到 macOS 的 `/`，所以那個檔就是 macOS 的 `/tmp/g923_telemetry.bin`。讀取器會自動先找 POSIX 共享記憶體，找不到再讀這個檔，兩種情況都不用改設定。
- ⚠️ 跨 Wine 邊界的記憶體一致性需**實機驗證**；若不穩，備援方案是改用 localhost UDP（外掛送、`g923d` 收）。

## 安裝

- **原生 macOS 版 ETS2**：把 `.dylib` 放進遊戲 app bundle 內 `.../bin/<arch>/plugins/`（沒有 `plugins` 就自己建）。
- **CrossOver/Whisky 的 Windows 版**：把 `.dll` 放進該 bottle 裡遊戲的 `bin/win_x64/plugins/`。
- 確認 Wine 的 `Z:` 仍對應 `/`（預設如此）。

## 驗證（不需方向盤）

外掛裝好、進遊戲開一台車後，用讀取工具確認資料有進來：

```bash
make tools
./build/g923_telemetry_dump          # 印出即時轉速/車速/路面粗糙度
```

看到轉速隨油門變動就代表遙測鏈路通了；接下來才把它接上 TrueForce 串流。
