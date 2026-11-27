# QQ-S Pro Marlin 韌體 — 版本對照

這個 repo 的版本標籤無法從推送環境建立，各版對應的 commit 列在這裡。

| 版本 | commit | 說明 |
|---|---|---|
| v3.2.22 | [`d19c29e`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/d19c29ec508125129657d62d4442ee0558700ff0) | v3.2.22: pause / resume / stop from the Wi-Fi module's web page (M2015) |
| v3.2.21 | [`c99d5d4`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/c99d5d4b45a458c34d8828650fb7730e96f8fa4a) | v3.2.21: wait for the SD card to finish writing before FatFs sync returns and before the card is reset |
| v3.2.20 | [`344defb`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/344defbaa90cc99ca47a6f9cc22e921758b42478) | v3.2.20: Wi-Fi upload gives the heaters back on every exit; kill screen shows the temperatures |
| v3.2.19 | [`7747c1b`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/7747c1b2b7d999e89481943f263d307c64079887) | v3.2.19: ESP flashing sends a block again when it arrived damaged |
| v3.2.18 | [`f5f477c`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/f5f477cc431d72daf0720f48134b9a01779931f2) | v3.2.18: probe gate before every probe job |
| v3.2.17 | [`d18e74a`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/d18e74acd0c6fb2e29bfcee359ebf01bd47d546e) | v3.2.17: probe tools only on a cold nozzle; shorter probe overrun |
| v3.2.16 | [`4ac2c84`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/4ac2c840c098e6f1086db7a536c0a47c850a7ba7) | v3.2.16: homing counts a tower done by its endstop hit flag too |
| v3.2.15 | [`c565dc0`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/c565dc0dfead862a2dcff9f61277160d43fca74e) | v3.2.15: PID asks "save?" in the middle; no flickering back button |
| v3.2.14 | [`6626558`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/66265587d2a907094552d16d8cb5b668337c80fd) | qqs_tools/lfn_test: host test of the UTF-8 long-name reader |
| v3.2.13 | [`914094c`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/914094caec7bedab77d832cab5e45f3e5d66ad35) | v3.2.13: M2012 status lines for the Wi-Fi module's web page |
| v3.2.12 | [`b6c6169`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/b6c6169cc99e351ade83453bd1459f0b5c4f0eda) | v3.2.12: ESP backup resumes after a lost packet; PID forearm |
| v3.2.11 | [`1a67578`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/1a67578926389613cfd8238d4a0cdaf9f20850f4) | v3.2.11: animated probe and PID screens from George's drawings |
| v3.2.10 | [`2b36ed8`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/2b36ed856770d0af387eb0428169a12473b91dd2) | v3.2.10: one go for the magnetic probe (M2007 A): tap the probe, countdown, G33 + G29 with the probe held on |
| v3.2.9 | [`c93ed71`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/c93ed71c939382e03d2d322ea84026ace96ffadc) | v3.2.9: short test G-code names (8.3 friendly on the SD browser) |
| v3.2.8 | [`81b873b`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/81b873bfedee29a86bc71256a04669e42907c2b9) | v3.2.8: wait screen also leaves Marlin's touch calibration alone when opened from settings / long press |
| v3.2.7 | [`64af4ae`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/64af4ae979a8eed12b8f3e42d4516bfd4ae1643a) | v3.2.7: make room: Marlin's own menus and status-screen icons are no longer built (-93.5 KB) |
| v3.2.6 | [`a9a80ba`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/a9a80ba98f6504bbaf29cac7c4fb736f8e0b1ff0) | v3.2.6: back up / restore / flash the MKS Wi-Fi ESP8266 from the printer (no USB-serial adapter) |
| v3.2.5 | [`7c091e3`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/7c091e3b63b3fca4f0a7cdeeb6fb662ea0c35537) | v3.2.5: Wi-Fi setup on the printer: scan + password keyboard, hotspot (AP) mode; parser keeps 0xA5 inside binary packets |
| v3.2.4 | [`a1e564a`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/a1e564aab227c66c204e2a8084986ca80a63d73a) | v3.2.4: Wi-Fi upload: turn the DMA1 clock on; save buttons confirm on the button |
| v3.2.3 | [`5f9f617`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/5f9f61733ae7cec3853c79fb1d1ff3607f952ec9) | v3.2.3: Wi-Fi diagnostics tab (no USB log needed) |
| v3.2.2 | [`3bf4ea4`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/3bf4ea4f792eac051a19ac87fa1a42a24d7ccbcd) | v3.2.2: manual calibration puts the heaters back when it ends |
| v3.2.1 | [`6d54b79`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/6d54b79bddd217eacb49469005b92e6da5011fda) | v3.2.1: G-code terminal, PID autotune page, machine parameters, feature switches |
| v3.2.0 | [`2e9ad9a`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/2e9ad9ae42c174d33ad2847b35b9f2215d839dd9) | v3.2.0: manual delta calibration, Z offset page, end-of-print babystep prompt, sensor page + endstop repeatability |
| v3.1.9 | [`4ceaabf`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/4ceaabf2dc074c093ea1f8108b49f7d69d6c3e94) | v3.1.9: animation ticks redraw only the moving parts |
| v3.1.8 | [`5e4e239`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/5e4e239acf7ea3079ef3f545e5934a72a7d8a57f) | v3.1.8: own file browser + confirm screen, clear on home/printing flip, heat-up pose |
| v3.1.7 | [`00ecce7`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/00ecce7b4ef4116619c1af32c1b51dcc73442e5a) | v3.1.7: printing animation (she paints a flower along a toolpath), new printing layout, M2010 timing |
| v3.1.6 | [`3f2d741`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/3f2d741e2a6cc005abfca62d0230a7f21e3bd2ba) | v3.1.6: Bowden calibration limit 900 -> 1500 mm, EXTRUDE_MAXLENGTH 1600 |
| v3.1.5 | [`d513615`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/d513615069d73c2b9e66503ab08a6748ae55a685) | v3.1.5: Bowden feed as one long move per speed (M410 to cut), touch pressure threshold 40, touch log |
| v3.1.4 | [`418b3e2`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/418b3e2222ef514341b78388d456d259b58ea23d) | v3.1.4: self-review fixes |
| v3.1.3 | [`87d25e2`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/87d25e2ec89372e05cb38afb1092f9fd2a3471c9) | v3.1.3: touch debounce for QQS buttons, Bowden stop can't be doubled into an exit |
| v3.1.2 | [`0bc567c`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/0bc567c76f771fee6b78d3c4883d17b0578d7be8) | v3.1.2: Bowden calibration two-speed feed (30 / 5 mm/s), stop escape hatch |
| v3.1.1 | [`3701ba3`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/3701ba35ba4bf3c4d90e916872f608bad80beefa) | v3.1.1: live screens keep their handler running (temps, position, Bowden feed did not update) |
| v3.1 | [`e4721ae`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/e4721aeaa7556169db6710dadc8cbfa2efccf691) | v3.1: move, filament (+Bowden calibration), QQS pause/park, 12-tile settings page |
| v3.0 | [`20d8df2`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/20d8df27365bbf816adf4d7297933c7b0c37aad2) | v3.0: QQS Traditional Chinese touch UI (home, printing, editors, stop confirm), own boot screen |
| fb4s_base | [`926a986`](https://github.com/Georgeeee6yt/qqsp-marlin/commit/926a98666a5b7b8f54fc5301d2b291679ac9a102) | Base: Sergey1560/Marlin_FB4S, branch FB4S_WIFI @ 99d168acb5e2bb90accc232115b6ee61a7868fc1 |

## 在本機補上標籤

```sh
git tag fb4s_base 926a98666a5b7b8f54fc5301d2b291679ac9a102
git tag v3.0 20d8df27365bbf816adf4d7297933c7b0c37aad2
git tag v3.1 e4721aeaa7556169db6710dadc8cbfa2efccf691
git tag v3.1.1 3701ba35ba4bf3c4d90e916872f608bad80beefa
git tag v3.1.2 0bc567c76f771fee6b78d3c4883d17b0578d7be8
git tag v3.1.3 87d25e2ec89372e05cb38afb1092f9fd2a3471c9
git tag v3.1.4 418b3e2222ef514341b78388d456d259b58ea23d
git tag v3.1.5 d513615069d73c2b9e66503ab08a6748ae55a685
git tag v3.1.6 3f2d741e2a6cc005abfca62d0230a7f21e3bd2ba
git tag v3.1.7 00ecce7b4ef4116619c1af32c1b51dcc73442e5a
git tag v3.1.8 5e4e239acf7ea3079ef3f545e5934a72a7d8a57f
git tag v3.1.9 4ceaabf2dc074c093ea1f8108b49f7d69d6c3e94
git tag v3.2.0 2e9ad9ae42c174d33ad2847b35b9f2215d839dd9
git tag v3.2.1 6d54b79bddd217eacb49469005b92e6da5011fda
git tag v3.2.2 3bf4ea4f792eac051a19ac87fa1a42a24d7ccbcd
git tag v3.2.3 5f9f61733ae7cec3853c79fb1d1ff3607f952ec9
git tag v3.2.4 a1e564aab227c66c204e2a8084986ca80a63d73a
git tag v3.2.5 7c091e3b63b3fca4f0a7cdeeb6fb662ea0c35537
git tag v3.2.6 a9a80ba98f6504bbaf29cac7c4fb736f8e0b1ff0
git tag v3.2.7 64af4ae979a8eed12b8f3e42d4516bfd4ae1643a
git tag v3.2.8 81b873bfedee29a86bc71256a04669e42907c2b9
git tag v3.2.9 c93ed71c939382e03d2d322ea84026ace96ffadc
git tag v3.2.10 2b36ed856770d0af387eb0428169a12473b91dd2
git tag v3.2.11 1a67578926389613cfd8238d4a0cdaf9f20850f4
git tag v3.2.12 b6c6169cc99e351ade83453bd1459f0b5c4f0eda
git tag v3.2.13 914094caec7bedab77d832cab5e45f3e5d66ad35
git tag v3.2.14 66265587d2a907094552d16d8cb5b668337c80fd
git tag v3.2.15 c565dc0dfead862a2dcff9f61277160d43fca74e
git tag v3.2.16 4ac2c840c098e6f1086db7a536c0a47c850a7ba7
git tag v3.2.17 d18e74acd0c6fb2e29bfcee359ebf01bd47d546e
git tag v3.2.18 f5f477cc431d72daf0720f48134b9a01779931f2
git tag v3.2.19 7747c1b2b7d999e89481943f263d307c64079887
git tag v3.2.20 344defbaa90cc99ca47a6f9cc22e921758b42478
git tag v3.2.21 c99d5d4b45a458c34d8828650fb7730e96f8fa4a
git tag v3.2.22 d19c29ec508125129657d62d4442ee0558700ff0
git push origin --tags
```
