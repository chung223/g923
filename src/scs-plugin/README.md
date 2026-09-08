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

## 編譯（macOS，給原生 Mac 版遊戲用）

```bash
make scs-plugin SCS_SDK=/path/to/scs_sdk
# 產出 build/g923_telemetry.dylib
```

## 安裝

把產出的外掛放進遊戲的 `plugins/` 資料夾：

- **原生 macOS 版 ETS2**：在遊戲的 app bundle 內
  `Euro Truck Simulator 2.app/Contents/.../bin/<arch>/plugins/`（若無 `plugins` 就自己建）。
- **透過 CrossOver/Whisky 跑的 Windows 版**：需要 **Windows 版外掛（.dll）**，不是這個 `.dylib`。
  要在 Windows 上（或用 mingw 交叉編譯）用同一份 `g923_scs_plugin.c` + SCS SDK 編成 `g923_telemetry.dll`，
  放進 Windows 遊戲的 `bin/win_x64/plugins/`。這條之後再補。

## 驗證（不需方向盤）

外掛裝好、進遊戲開一台車後，用讀取工具確認資料有進來：

```bash
make tools
./build/g923_telemetry_dump          # 印出即時轉速/車速/路面粗糙度
```

看到轉速隨油門變動就代表遙測鏈路通了；接下來才把它接上 TrueForce 串流。
