# FLSUN QQ-S Pro 自製 Marlin 韌體

給 **FLSUN QQ-S Pro**（Delta，HiSpeed 主板，STM32F103VE）用的 Marlin 2.1.2.1 韌體，有全中文觸控介面，原廠的 MKS Wi-Fi 模組也可以照常使用。

目前版本 **v3.2.22**。各版本跟 commit 的對照請看 [`VERSIONS.md`](VERSIONS.md)。

> ⚠️ 這是個人改機用的韌體，只在一台機器上用過，而且只出 **BMG 擠出機**版本（E 反向、415 步/mm）。刷之前先用 `M503` 把原本的設定記下來。

## 功能

- **中文觸控介面**（`Marlin/src/lcd/tft/qqs/`）：主畫面、檔案瀏覽（新的檔案排在前面）、列印畫面，還有動畫（圖是自己畫的）
- **校準工具**：手動 Delta 校準、Z 偏移、限位開關一致性測試、PID 調校（結束時在畫面中間問要不要存）、Bowden 管長校正
- **磁吸探頭工具**：探頭高度、重複性、G33 Delta 自動校準、G29 網格、一鍵完成全部
  - 裝上探頭後先做「探頭閘門」：在中心量 5 次。範圍 ≤ 0.02 mm 直接繼續，0.02～0.04 mm 先問要不要繼續，超過 0.04 mm 就歸零並提示重新安裝
  - 噴嘴降到 50 °C 以下才能用，用的時候不准加熱噴嘴
- **歸零跟原廠一樣**：三塔一起快速上升，三塔一起退開，再慢速碰一次，最後往下 2 mm 放開開關
- **Wi-Fi**（MKS ESP8266 模組）：
  - 切片軟體上傳後直接開印
  - 在印表機上掃描網路、輸入密碼、開熱點
  - 從印表機備份或刷寫 ESP8266 的韌體（不需要 USB 轉序列）
  - 搭配 [qqsp-mks-wifi](https://github.com/Georgeeee6yt/qqsp-mks-wifi) 的網頁，可以看列印狀態、從網頁選 SD 卡的檔案來印（中文長檔名也行），列印中可以暫停、繼續、停止
- **Smooth Linear Advance**（從上游 Marlin 移植，加上 S 曲線支援），預設 K = 0
- `M2011` 效能量測（步進中斷、planner 的 CPU 用量）
- Marlin 原本的選單已經整個拿掉，換成這套介面

## 刷韌體

1. 把 `Robin_mini.bin` 放到 SD 卡根目錄，開機就會自動更新。編譯時會產生已經加密好的檔案。
2. 第一次刷：如果出現「EEPROM 資料不符」，按「存成預設」，然後重新做一次校準。

## 編譯

用 PlatformIO：

```sh
pio run -e flsun_hispeedv1
# 產出 .pio/build/flsun_hispeedv1/Robin_mini.bin
```

- Flash 上限是 491,520 bytes（512 KB 扣掉 bootloader 的 28 KB），v3.2.18 用了約 404 KB。看 bin 檔的大小就好，pio 顯示的百分比不準。
- 中文字型是 `qqs_tools/gen_assets.py` 從原始碼裡實際用到的字產生的，有加新字的話要重新產生。

## 程式碼來源

| 部分 | 來源 |
|---|---|
| Marlin 核心 | [Marlin](https://github.com/MarlinFirmware/Marlin) 2.1.2.1 |
| MKS Wi-Fi、SDIO、FatFs | [Sergey1560/Marlin_FB4S](https://github.com/Sergey1560/Marlin_FB4S) 的 `FB4S_WIFI` 分支（`99d168a`，原本的說明在 [`README_FB4S.md`](README_FB4S.md)） |
| QQ-S Pro 機器設定 | Marlin 官方 Configurations 的 `delta/FLSUN/QQS-Pro` 範例 |
| Smooth Linear Advance | 上游 Marlin bugfix-2.1.x（dbuezas），經 BIQU B1 版本移植 |
| 中文介面、工具、ESP 燒錄、各種修正 | George＋Claude |

這個 repo 的第一個 commit 是 FB4S 當時的原始碼（沒有帶上游的歷史紀錄），之後的 commit 才是 QQ-S Pro 的修改。

## 授權

GPL-3.0，跟 Marlin 一樣，見 [`LICENSE`](LICENSE)。
