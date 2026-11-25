/**
 * QQS-Pro v3.2.5 UI — Wi-Fi tab of machine info: status, network scan + password keyboard + connect,
 * switch to the module's own hotspot (AP mode), and the detailed diagnostics page.
 *
 * The MKS ESP keeps its network settings in its own flash. A type 0 packet with different settings
 * makes it save them and restart (about 1 s); joining a network then takes a few seconds more. If it
 * cannot join within ~25 s it falls back to an open hotspot "MKSWIFI" + 4 hex digits of its MAC.
 *
 * Every Chinese string here is collected by qqs_tools/gen_assets.py (re-run it after text changes).
 * Lines that draw with F_L must keep that token on the same line as the literal.
 */
#include "../../../inc/MarlinConfig.h"

#if ENABLED(QQS_UI)

#include "qqs_ui_priv.h"
#include "../tft.h"
#include "../touch.h"
#include "qqs_wifi_diag.h"
#ifdef MKS_WIFI
  extern uint8_t mks_wifi_ip[4];
  extern uint8_t mks_wifi_net_state;
  #include "../../../module/mks_wifi/qqs_espflash.h"
  void espf_scan_sd(uint32_t sizes[5]);
#endif
#if HAS_BUZZER
  #include "../../../libs/buzzer.h"
#endif

namespace qqs {

  #define QQS_AP_NAME "QQS-Pro"
  #define WIFI_TIMEOUT_MS 20000

  static void diag_screen();
  #ifdef MKS_WIFI
    static void go_scan();
    static void go_ap();
    static void go_fwm();
  #endif

  // Network name for display: ASCII as is, one '?' per non-ASCII character, "…" when too wide.
  static void fit(char *o, const char *s, const qqs_font_t &f, const uint16_t maxw) {   // o: 48 bytes
    uint8_t n = 0;
    for (uint8_t i = 0; s[i] && n < 40; i++) {
      const uint8_t c = s[i];
      if (c >= 0x80 && c < 0xC0) continue;          // UTF-8 continuation byte
      o[n++] = (c < 0x20 || c > 0x7E) ? '?' : char(c);
    }
    o[n] = 0;
    if (width(f, o) <= maxw) return;
    const uint16_t ew = width(f, "…");
    while (n && width(f, o) + ew > maxw) o[--n] = 0;
    strcat(o, "…");
  }

  static bool wifi_ok() {
    #ifdef MKS_WIFI
      return !job_active();
    #else
      return false;
    #endif
  }

  // =========================================================================
  // Wi-Fi tab
  // =========================================================================

  static void wifi_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 1000)) return;

    touch.clear();
    header("機器資訊", go_settings);
    info_tabs(2);
    canvas(0, HEADER_H + 32, TFT_WIDTH, 92, C_BG);
    const qqs_wifi_diag_t &d = qqs_wdiag;
    char v[80], nm[48];
    int16_t y = 2;
    auto row = [&](const char *label, const char *val, const uint16_t col = C_TEXT) {
      text(6, y, F_S, C_MUTED, C_BG, label);
      text(70, y, F_S, col, C_BG, val);
      y += 18;
    };

    #ifdef MKS_WIFI
      const qqs_wifi_net_t &n = qqs_wnet;
      if (!n.reports) row("狀態", "還沒收到模組回報", C_WARN);
      else if (n.mode == 1) {
        sprintf_P(v, PSTR("熱點模式　網址 %u.%u.%u.%u"), mks_wifi_ip[0], mks_wifi_ip[1], mks_wifi_ip[2], mks_wifi_ip[3]);
        row("狀態", v, C_OK);
      }
      else if (mks_wifi_net_state == 1) {
        sprintf_P(v, PSTR("已連線　%u.%u.%u.%u"), mks_wifi_ip[0], mks_wifi_ip[1], mks_wifi_ip[2], mks_wifi_ip[3]);
        row("狀態", v, C_OK);
      }
      else row("狀態", n.state == 0x0E ? "異常" : "還沒連上網路", n.state == 0x0E ? C_STOP : C_WARN);

      if (n.reports) {
        fit(nm, n.ssid, F_S, n.mode == 1 ? 150 : 240);
        if (n.mode == 1) { sprintf_P(v, PSTR("%s（%s）"), nm, n.key[0] ? "有密碼" : "開放"); row("熱點名稱", v); }
        else row("網路", *nm ? nm : "（沒有設定）");
      }
      else row("網路", "—", C_DIM);
    #endif

    sprintf_P(v, PSTR("網路 %lu  G-code %lu  檔案 %lu  壞 %lu"), (unsigned long)d.rx_pkt[0], (unsigned long)d.rx_pkt[1],
              (unsigned long)d.rx_pkt[2], (unsigned long)(d.rx_bad + d.rx_other));
    row("收到", v);
    sprintf_P(v, PSTR("%lu 行　逾時 %lu"), (unsigned long)d.tx_lines, (unsigned long)d.tx_timeouts);
    row("送出", v, d.tx_timeouts ? C_WARN : C_TEXT);
    if (!d.up_count) row("上次上傳", "還沒有上傳過", C_DIM);
    else {
      fit(nm, d.up_name, F_S, 200);
      sprintf_P(v, PSTR("%s　%s"), d.up_reason == 'O' ? "成功" : "失敗", nm);
      row("上次上傳", v, d.up_reason == 'O' ? C_OK : C_STOP);
    }

    canvas(0, HEADER_H + 124, TFT_WIDTH, 22, C_BG);
    text_c(0, 3, TFT_WIDTH, F_S, C_DIM, C_BG, job_active() ? "列印中不能改 Wi-Fi 設定" : "換網路：掃描網路 → 點選 → 輸入密碼");

    Btn a; a.icon = &qqs_i18_wifi; a.label = "掃描網路"; a.fg = C_ACCENT; a.border = C_ACCENT; a.enabled = wifi_ok();
    #ifdef MKS_WIFI
      constexpr screenFunc_t scan_cb = go_scan, ap_cb = go_ap, fwm_cb = go_fwm;
    #else
      constexpr screenFunc_t scan_cb = nullptr, ap_cb = nullptr, fwm_cb = nullptr;
    #endif
    button(4, 180, 75, 56, a, scan_cb);
    Btn b; b.icon = &qqs_i18_printer; b.label = "熱點模式"; b.enabled = wifi_ok();
    button(83, 180, 75, 56, b, ap_cb);
    Btn f; f.icon = &qqs_i18_device_floppy; f.label = "模組韌體"; f.enabled = wifi_ok();
    button(162, 180, 75, 56, f, fwm_cb);
    Btn c; c.icon = &qqs_i18_list; c.label = "詳細診斷";
    button(241, 180, 75, 56, c, []{ ui.goto_screen(diag_screen); });
  }

  void go_wifi() { ui.goto_screen(wifi_screen); }

  // =========================================================================
  // Detailed diagnostics (what the v3.2.3 tab showed)
  // =========================================================================

  static void diag_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 1000)) return;

    touch.clear();
    header("Wi-Fi 診斷", go_wifi);
    canvas(0, HEADER_H, TFT_WIDTH, TFT_HEIGHT - HEADER_H, C_BG);
    const qqs_wifi_diag_t &d = qqs_wdiag;
    char v[64];
    int16_t y = 4;
    auto row = [&](const char *label, const char *val, const uint16_t col = C_TEXT) {
      text(6, y, F_S, C_MUTED, C_BG, label);
      text(70, y, F_S, col, C_BG, val);
      y += 17;
    };

    sprintf_P(v, PSTR("網路 %lu  G-code %lu  檔案 %lu  壞 %lu  雜 %lu"), (unsigned long)d.rx_pkt[0], (unsigned long)d.rx_pkt[1],
              (unsigned long)d.rx_pkt[2], (unsigned long)(d.rx_bad + d.rx_other), (unsigned long)d.rx_stray);
    row("收到", v);
    row("最近收到", *d.last_in[d.last_in_i] ? d.last_in[d.last_in_i] : "—", C_COLD);
    sprintf_P(v, PSTR("%lu 行　逾時 %lu"), (unsigned long)d.tx_lines, (unsigned long)d.tx_timeouts);
    row("送出", v, d.tx_timeouts ? C_WARN : C_TEXT);
    row("最近送出", *d.last_out ? d.last_out : "—", C_COLD);

    if (!d.up_count) { row("上傳", "還沒有上傳過", C_DIM); return; }
    sprintf_P(v, PSTR("%s  %lu B"), d.up_name, (unsigned long)d.up_size);
    row("上次上傳", v);
    const char *why;
    switch (d.up_reason) {
      case 'O': why = "成功"; break;
      case 'T': why = d.up_pk ? "中途沒資料了（逾時）" : "完全沒收到資料（逾時）"; break;
      case 'H': why = "封包開頭錯（速率／雜訊）"; break;
      case 'S': why = "封包跳號（漏收）"; break;
      case 'W': why = "寫 SD 失敗"; break;
      case 'E': why = "DMA 錯誤"; break;
      case 'Z': why = "收完但大小不符"; break;
      case 'M': why = "SD 掛載失敗"; break;
      case 'F': why = "開檔失敗"; break;
      default:  why = "進行中／未知"; break;
    }
    sprintf_P(v, PSTR("%c %s"), d.up_reason, why);
    row("結果", v, d.up_reason == 'O' ? C_OK : C_STOP);
    sprintf_P(v, PSTR("封包 %lu　收 %lu　寫 %lu"), (unsigned long)d.up_pk, (unsigned long)d.up_recv, (unsigned long)d.up_written);
    row("數量", v);
    sprintf_P(v, PSTR("剩 %u／1024　SR %04X　IO4 %c"), d.up_cndtr, d.up_sr, d.up_io4 ? 'H' : 'L');
    row("DMA", v);
    sprintf_P(v, PSTR("%02X %02X %02X %02X %02X %02X %02X %02X"), d.up_first[0], d.up_first[1], d.up_first[2], d.up_first[3],
              d.up_first[4], d.up_first[5], d.up_first[6], d.up_first[7]);
    row("開頭", v);
  }

#ifdef MKS_WIFI

  // =========================================================================
  // Shared state of the setup flow
  // =========================================================================

  static void scan_screen();
  static void kb_screen();
  static void ap_screen();
  static void pg_screen();

  static char ws_ssid[33];          // network being joined
  static char ws_key[64];           // password typed for it (63 max)
  static char ap_key[64];           // hotspot password ("" = open)
  static uint8_t kb_for = 0;        // keyboard is for: 0 the network password, 1 the hotspot password
  static bool kb_bad = false;       // hotspot password of 1..7 characters was refused
  static bool scan_from_ap = false; // the scan started in AP mode: the module dropped its hotspot to scan
  static millis_t ap_arm_ms = 0;    // first press of "switch to hotspot"
  static uint8_t ap_sig = 0xFF;     // what the hotspot page shows (0xFF = redraw)

  // =========================================================================
  // Network scan
  // =========================================================================

  static millis_t scan_ms = 0;
  static uint16_t scan_seq0 = 0;
  static uint8_t scan_page = 0;
  static uint32_t scan_sig = 0xFFFFFFFF;
  #define SCAN_ROWS 5

  static void scan_start() {
    qqs_wifi_scan();
    scan_ms = millis();
    scan_seq0 = qqs_wscan.seq;
    scan_page = 0;
    scan_sig = 0xFFFFFFFF;
    redraw();
  }
  static void scan_back() {
    if (scan_from_ap) { scan_from_ap = false; qqs_wifi_reset(); }   // bring its hotspot back
    go_wifi();
  }
  static uint8_t scan_pages() { return (qqs_wscan.n + SCAN_ROWS - 1) / SCAN_ROWS; }
  static void scan_prev() { if (scan_page) scan_page--; scan_sig = 0xFFFFFFFF; redraw(); }
  static void scan_next() { if (scan_page + 1 < scan_pages()) scan_page++; scan_sig = 0xFFFFFFFF; redraw(); }

  static void kb_open(const uint8_t purpose, const char *prefill);
  template<uint8_t R> static void scan_pick() {
    const uint8_t i = scan_page * SCAN_ROWS + R;
    if (i >= qqs_wscan.n) return;
    strcpy(ws_ssid, qqs_wscan.name[i]);
    const qqs_wifi_net_t &n = qqs_wnet;   // the same network as now: start from the password it already has
    kb_open(0, (n.mode == 2 && !strcmp(n.ssid, ws_ssid)) ? n.key : "");
  }

  static void bars(const int16_t x, const int16_t yb, const int8_t rssi) {
    const uint8_t lit = rssi >= -55 ? 4 : rssi >= -63 ? 3 : rssi >= -70 ? 2 : 1;
    for (uint8_t i = 0; i < 4; i++) {
      const int16_t h = 4 + i * 4;
      fill(x + i * 5, yb - h, 3, h, i < lit ? C_OK : C_DIM);
    }
  }

  static void scan_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    const bool draw = live(next_ms, 500);
    const bool got = qqs_wscan.seq != scan_seq0;
    const uint32_t secs = (millis() - scan_ms) / 1000;
    const uint32_t sig = got ? (0x10000UL | (uint32_t(qqs_wscan.seq) << 4) | scan_page) : _MIN(secs, 99UL);
    if (!draw || sig == scan_sig) return;
    scan_sig = sig;

    touch.clear();
    header("選擇網路", scan_back);
    canvas(0, HEADER_H, TFT_WIDTH, 154, C_BG);
    char v[64], nm[48];

    if (!got) {
      if (secs < 12) {
        text_c(0, 50, TFT_WIDTH, F_L, C_TEXT, C_BG, "掃描中…");
        sprintf_P(v, PSTR("%lu 秒"), (unsigned long)secs);
        text_c(0, 84, TFT_WIDTH, F_S, C_MUTED, C_BG, v);
      }
      else {
        text_c(0, 40, TFT_WIDTH, F_M, C_WARN, C_BG, "沒收到網路列表");
        text_c(0, 70, TFT_WIDTH, F_S, C_MUTED, C_BG, "附近沒有網路，或訊號都太弱");
        text_c(0, 88, TFT_WIDTH, F_S, C_MUTED, C_BG, "（模組不列出 −78 dBm 以下的網路）");
      }
    }
    else if (!qqs_wscan.n) text_c(0, 50, TFT_WIDTH, F_M, C_WARN, C_BG, "沒有找到網路");
    else {
      static const screenFunc_t pick[SCAN_ROWS] = { scan_pick<0>, scan_pick<1>, scan_pick<2>, scan_pick<3>, scan_pick<4> };
      const qqs_wifi_net_t &n = qqs_wnet;
      for (uint8_t r = 0; r < SCAN_ROWS; r++) {
        const uint8_t i = scan_page * SCAN_ROWS + r;
        if (i >= qqs_wscan.n) break;
        const int16_t y = HEADER_H + 3 + r * 30;
        canvas(4, y, 312, 28, C_TILE);
        frame(0, 0, 312, 28, C_BORDER);
        bars(8, 21, qqs_wscan.rssi[i]);
        sprintf_P(v, PSTR("%d dBm"), int(qqs_wscan.rssi[i]));
        const int16_t dw = width(F_S, v);
        text_r(304, 6, F_S, C_MUTED, C_TILE, v);
        const bool cur = n.mode == 2 && n.state == 0x0A && !strcmp(n.ssid, qqs_wscan.name[i]);
        int16_t room = 304 - dw - 8 - 32;
        if (cur) { text_r(304 - dw - 8, 6, F_S, C_OK, C_TILE, "已連線"); room -= width(F_S, "已連線") + 8; }
        fit(nm, qqs_wscan.name[i], F_M, room);
        text(32, 3, F_M, cur ? C_OK : C_TEXT, C_TILE, nm);
        touch.add_control(BUTTON, 4, y, 312, 28, (intptr_t)pick[r]);
      }
    }

    const int16_t by = HEADER_H + 158;
    const uint8_t pages = scan_pages();
    Btn p; p.label = "上一頁"; p.enabled = got && scan_page > 0;
    button(4, by, 98, 46, p, scan_prev);
    Btn s; s.label = "重新掃描"; s.fg = C_ACCENT; s.border = C_ACCENT;
    if (got && pages > 1) { sprintf_P(v, PSTR("第 %u／%u 頁"), scan_page + 1, pages); s.sub = v; }
    button(106, by, 108, 46, s, scan_start);
    Btn q; q.label = "下一頁"; q.enabled = got && scan_page + 1 < pages;
    button(218, by, 98, 46, q, scan_next);
  }

  // =========================================================================
  // Password keyboard (plain text; letters, capitals, digits + symbols, more symbols)
  // =========================================================================

  static char kb_buf[64];
  static uint8_t kb_len = 0, kb_mode = 0;
  static bool kb_field_dirty = false;

  // 3 rows x 10 per layout; ' ' = no key there. All printable ASCII is reachable (space has its own key).
  static const char kb_keys[4][31] = {
    "qwertyuiopasdfghjkl zxcvbnm   ",
    "QWERTYUIOPASDFGHJKL ZXCVBNM   ",
    "1234567890!@#$%^&*()-_=+.,:;/?",
    "\"'`~|\\[]{}<>                  "
  };

  static void kb_open(const uint8_t purpose, const char *prefill) {
    kb_for = purpose;
    kb_bad = false;
    strncpy(kb_buf, prefill, 63); kb_buf[63] = 0;
    kb_len = strlen(kb_buf);
    kb_mode = 0;
    ui.goto_screen(kb_screen);
  }

  static void kb_draw_field() {
    kb_field_dirty = false;
    canvas(0, HEADER_H + 2, TFT_WIDTH, 46, C_BG);
    char v[72], nm[48];
    if (kb_for == 0) {
      fit(nm, ws_ssid, F_S, 200);
      sprintf_P(v, PSTR("網路：%s"), nm);
      text(6, 0, F_S, C_MUTED, C_BG, v);
    }
    else text(6, 0, F_S, kb_bad ? C_STOP : C_MUTED, C_BG, kb_bad ? "熱點密碼要 8 個字以上，或留空" : "熱點密碼：留空＝開放網路，或 8 個字以上");
    if (kb_len) { sprintf_P(v, PSTR("%u 字"), kb_len); text_r(TFT_WIDTH - 6, 0, F_S, kb_len < 8 ? C_WARN : C_MUTED, C_BG, v); }

    fill(0, 18, TFT_WIDTH, 28, C_TILE);
    frame(0, 18, TFT_WIDTH, 28, C_ACCENT);
    const char *p = kb_buf;                       // show the end when it is longer than the field
    while (*p && width(F_M, p) > TFT_WIDTH - 46) p++;   // room for "…" and "_"
    sprintf_P(v, PSTR("%s%s_"), p != kb_buf ? "…" : "", p);
    text(6, 21, F_M, C_TEXT, C_TILE, v);
  }

  template<uint8_t I> static void kb_key() {
    const char c = kb_keys[kb_mode][I];
    if (c == ' ' || kb_len >= 63) return;
    kb_buf[kb_len++] = c; kb_buf[kb_len] = 0;
    kb_bad = false;
    kb_field_dirty = true;
  }
  static void kb_space() { if (kb_len < 63) { kb_buf[kb_len++] = ' '; kb_buf[kb_len] = 0; kb_field_dirty = true; } }
  static void kb_bs()    { if (kb_len) kb_buf[--kb_len] = 0; kb_field_dirty = true; }
  template<uint8_t M> static void kb_set_mode() { kb_mode = M; redraw(); }

  static void pg_start(const uint8_t kind);
  static void kb_ok() {
    if (kb_for == 1) {
      if (kb_len && kb_len < 8) { kb_bad = true; kb_field_dirty = true; return; }
      strcpy(ap_key, kb_buf);
      ap_arm_ms = 0; ap_sig = 0xFF;
      ui.goto_screen(ap_screen);
      return;
    }
    strcpy(ws_key, kb_buf);
    scan_from_ap = false;                     // the module restarts with the new settings anyway
    qqs_wifi_config(2, ws_ssid, ws_key);
    pg_start(0);
  }
  static void kb_back() {
    if (kb_for == 1) { ap_sig = 0xFF; ui.goto_screen(ap_screen); }
    else { scan_sig = 0xFFFFFFFF; ui.goto_screen(scan_screen); }
  }

  static void kb_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 60000)) {             // between full redraws only the field changes
      if (kb_field_dirty) kb_draw_field();
      return;
    }

    touch.clear();
    header(kb_for ? "熱點密碼" : "輸入密碼", kb_back);
    kb_draw_field();

    static const screenFunc_t key_cb[30] = {
      kb_key<0>,  kb_key<1>,  kb_key<2>,  kb_key<3>,  kb_key<4>,  kb_key<5>,  kb_key<6>,  kb_key<7>,  kb_key<8>,  kb_key<9>,
      kb_key<10>, kb_key<11>, kb_key<12>, kb_key<13>, kb_key<14>, kb_key<15>, kb_key<16>, kb_key<17>, kb_key<18>, kb_key<19>,
      kb_key<20>, kb_key<21>, kb_key<22>, kb_key<23>, kb_key<24>, kb_key<25>, kb_key<26>, kb_key<27>, kb_key<28>, kb_key<29>
    };
    constexpr int16_t ky = HEADER_H + 52, kp = 32, kh = 32, rp = 35;
    char k[2] = { 0, 0 };
    for (uint8_t r = 0; r < 3; r++) {
      uint8_t cnt = 0;
      for (uint8_t c = 0; c < 10; c++) if (kb_keys[kb_mode][r * 10 + c] != ' ') cnt++;
      const int16_t x0 = (TFT_WIDTH - cnt * kp) / 2 + 1;
      uint8_t slot = 0;
      for (uint8_t c = 0; c < 10; c++) {
        const uint8_t i = r * 10 + c;
        k[0] = kb_keys[kb_mode][i];
        if (k[0] == ' ') continue;
        Btn b; b.label = k;
        button(x0 + slot * kp, ky + r * rp, kp - 2, kh, b, key_cb[i]);
        slot++;
      }
    }

    const int16_t y4 = ky + 3 * rp + 1, h4 = TFT_HEIGHT - y4 - 2;
    static const char * const mode_lab[4] = { "abc", "ABC", "123", "#+=" };
    static const screenFunc_t mode_cb[4] = { kb_set_mode<0>, kb_set_mode<1>, kb_set_mode<2>, kb_set_mode<3> };
    for (uint8_t m = 0; m < 4; m++) {
      Btn b; b.label = mode_lab[m]; b.font = &F_S;
      if (m == kb_mode) { b.fg = C_ACCENT; b.border = C_ACCENT; }
      button(1 + m * 40, y4, 38, h4, b, mode_cb[m]);
    }
    { Btn b; b.label = "空白"; b.font = &F_S; button(161, y4, 48, h4, b, kb_space); }
    { Btn b; b.label = "←"; b.fg = C_WARN; button(211, y4, 40, h4, b, kb_bs); }
    { Btn b; b.label = kb_for ? "確定" : "連線"; b.fg = C_ACCENT; b.border = C_ACCENT; button(253, y4, 66, h4, b, kb_ok); }
  }

  // =========================================================================
  // Hotspot (AP mode)
  // =========================================================================

  static void go_ap() {
    const qqs_wifi_net_t &n = qqs_wnet;      // already the hotspot: show the password it has
    if (n.mode == 1 && !strcmp(n.ssid, QQS_AP_NAME)) strcpy(ap_key, n.key);
    ap_arm_ms = 0;
    ap_sig = 0xFF;
    ui.goto_screen(ap_screen);
  }
  static void ap_set_key() { kb_open(1, ap_key); }
  static void ap_go() {
    if (!ap_arm_ms || ELAPSED(millis(), ap_arm_ms + 3000)) { ap_arm_ms = millis(); ap_sig = 0xFF; redraw(); return; }
    ap_arm_ms = 0;
    scan_from_ap = false;
    qqs_wifi_config(1, QQS_AP_NAME, ap_key);
    pg_start(1);
  }

  static void ap_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    const bool draw = live(next_ms, 500);
    const bool armed = ap_arm_ms && PENDING(millis(), ap_arm_ms + 3000);
    if (!armed) ap_arm_ms = 0;
    const qqs_wifi_net_t &n = qqs_wnet;
    const bool already = n.mode == 1 && !strcmp(n.ssid, QQS_AP_NAME) && !strcmp(n.key, ap_key);
    const uint8_t sig = uint8_t(armed) | (uint8_t(already) << 1);
    if (!draw || sig == ap_sig) return;
    ap_sig = sig;

    touch.clear();
    header("熱點模式", go_wifi);
    canvas(0, HEADER_H, TFT_WIDTH, 144, C_BG);
    text(8, 4, F_S, C_MUTED, C_BG, "印表機自己開一個 Wi-Fi，手機或電腦直接連它，");
    text(8, 20, F_S, C_MUTED, C_BG, "不用經過家裡的路由器。");
    char nm[48];
    auto row = [&](const int16_t y, const char *label, const char *val, const uint16_t col) {
      text(8, y, F_M, C_MUTED, C_BG, label);
      text(70, y, F_M, col, C_BG, val);
    };
    row(40, "名稱", QQS_AP_NAME, C_TEXT);
    if (ap_key[0]) fit(nm, ap_key, F_M, 240);
    row(62, "密碼", ap_key[0] ? nm : "無（開放網路）", ap_key[0] ? C_TEXT : C_WARN);
    row(84, "網址", "192.168.4.1", C_TEXT);
    if (already) text(8, 112, F_S, C_OK, C_BG, "目前已經是這個熱點。");
    else {
      text(8, 110, F_S, C_WARN, C_BG, "切換後會離開目前的網路；");
      text(8, 126, F_S, C_WARN, C_BG, "要回家用網路，請用「掃描網路」重新連。");
    }

    Btn k; k.icon = &qqs_i18_lock_open; k.label = "設定密碼";
    button(4, 180, 154, 56, k, ap_set_key);
    Btn g; g.icon = &qqs_i18_printer; g.label = armed ? "再按一次確認" : "切換到熱點";
    if (armed) { g.fg = C_STOP; g.border = C_STOP; g.bg = C_STOPBG; }
    else { g.fg = C_ACCENT; g.border = C_ACCENT; }
    button(162, 180, 154, 56, g, ap_go);
  }

  // =========================================================================
  // Progress: joining a network (kind 0) / switching to the hotspot (kind 1)
  // =========================================================================

  static uint8_t pg_kind = 0;
  static millis_t pg_ms = 0;
  static uint16_t pg_rep0 = 0;
  static uint8_t pg_result = 0;     // 0 waiting, 1 done, 2 not yet after 20 s, 3 fell back to its own hotspot
  static uint32_t pg_sig = 0xFFFFFFFF;

  static void pg_start(const uint8_t kind) {
    pg_kind = kind;
    pg_ms = millis();
    pg_rep0 = qqs_wnet.reports;
    pg_result = 0;
    pg_sig = 0xFFFFFFFF;
    ui.goto_screen(pg_screen);
  }
  static void pg_retype() { kb_open(0, ws_key); }

  static void pg_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    const bool draw = live(next_ms, 500);

    // Judge every call: success may come after the 20 s note is already shown
    const qqs_wifi_net_t &n = qqs_wnet;
    const millis_t el = millis() - pg_ms;
    const bool fresh = n.reports != pg_rep0;
    if (pg_result != 1 && fresh) {
      const bool ok = pg_kind == 0
        ? (n.mode == 2 && n.state == 0x0A && !strcmp(n.ssid, ws_ssid) && !strcmp(n.key, ws_key))
        : (n.mode == 1 && !strcmp(n.ssid, QQS_AP_NAME) && !strcmp(n.key, ap_key));
      if (ok) {
        pg_result = 1;
        #if HAS_BUZZER
          BUZZ(80, 2000);
        #endif
      }
      else if (pg_kind == 0 && el > 8000 && n.mode == 1 && !strncmp(n.ssid, "MKSWIFI", 7)) pg_result = 3;
    }
    if (pg_result == 0 && el > WIFI_TIMEOUT_MS) pg_result = 2;

    const uint32_t sig = (uint32_t(pg_result) << 24) | (pg_result ? 0 : _MIN(el / 1000, 999UL));
    if (!draw || sig == pg_sig) return;
    pg_sig = sig;

    touch.clear();
    header(pg_kind ? "熱點模式" : "連線", go_wifi);
    canvas(0, HEADER_H, TFT_WIDTH, 144, C_BG);
    char v[96], nm[48], ip[20];
    sprintf_P(ip, PSTR("%u.%u.%u.%u"), mks_wifi_ip[0], mks_wifi_ip[1], mks_wifi_ip[2], mks_wifi_ip[3]);
    fit(nm, pg_kind ? QQS_AP_NAME : ws_ssid, F_M, 300);

    switch (pg_result) {
      case 0:
        text_c(0, 14, TFT_WIDTH, F_L, C_TEXT, C_BG, pg_kind ? "切換中…" : "連線中…");
        text_c(0, 50, TFT_WIDTH, F_M, C_TEXT, C_BG, nm);
        sprintf_P(v, PSTR("%lu 秒"), (unsigned long)(el / 1000));
        text_c(0, 78, TFT_WIDTH, F_S, C_MUTED, C_BG, v);
        text_c(0, 100, TFT_WIDTH, F_S, C_DIM, C_BG, !fresh ? "模組重開中，等它回報…" : (pg_kind ? "等熱點開好…" : "模組正在連這個網路…"));
        break;
      case 1:
        text_c(0, 14, TFT_WIDTH, F_L, C_OK, C_BG, pg_kind ? "熱點已開啟" : "已連線");
        text_c(0, 50, TFT_WIDTH, F_M, C_TEXT, C_BG, nm);
        if (pg_kind) {
          sprintf_P(v, PSTR("網址 %s　密碼 %s"), ip, ap_key[0] ? "（剛剛設的）" : "無");
          text_c(0, 80, TFT_WIDTH, F_S, C_TEXT, C_BG, v);
          text_c(0, 100, TFT_WIDTH, F_S, C_MUTED, C_BG, "切片軟體的印表機位址要改成這個網址");
        }
        else {
          sprintf_P(v, PSTR("IP %s"), ip);
          text_c(0, 80, TFT_WIDTH, F_M, C_TEXT, C_BG, v);
          text_c(0, 108, TFT_WIDTH, F_S, C_MUTED, C_BG, "IP 跟以前不同的話，切片軟體裡也要改");
        }
        break;
      case 2:
        text_c(0, 14, TFT_WIDTH, F_L, C_STOP, C_BG, pg_kind ? "還沒確認到切換" : "20 秒還沒連上");
        text_c(0, 50, TFT_WIDTH, F_M, C_TEXT, C_BG, nm);
        if (pg_kind) text_c(0, 82, TFT_WIDTH, F_S, C_MUTED, C_BG, "模組可能還在重開，稍後到 Wi-Fi 分頁看狀態");
        else {
          text_c(0, 82, TFT_WIDTH, F_S, C_MUTED, C_BG, "請確認密碼（大小寫有分）");
          text_c(0, 100, TFT_WIDTH, F_S, C_DIM, C_BG, "模組還會再試一下，連上的話這頁會自動更新");
        }
        break;
      default:
        text_c(0, 14, TFT_WIDTH, F_L, C_STOP, C_BG, "連不上這個網路");
        text_c(0, 50, TFT_WIDTH, F_M, C_TEXT, C_BG, nm);
        fit(nm, n.ssid, F_S, 120);
        sprintf_P(v, PSTR("模組已改開自己的熱點 %s（開放）"), nm);
        text_c(0, 82, TFT_WIDTH, F_S, C_MUTED, C_BG, v);
        text_c(0, 100, TFT_WIDTH, F_S, C_MUTED, C_BG, "多半是密碼不對，可以重打再試");
        break;
    }

    if (pg_result == 1 || pg_kind == 1) {
      Btn b; b.icon = pg_result == 1 ? &qqs_i18_check : &qqs_i18_chevron_left; b.label = pg_result == 1 ? "完成" : "返回"; b.fg = C_ACCENT; b.border = C_ACCENT;
      button(83, 180, 154, 56, b, go_wifi);
    }
    else {
      Btn r; r.icon = &qqs_i18_lock_open; r.label = "重打密碼"; r.enabled = pg_result != 0;
      button(4, 180, 154, 56, r, pg_retype);
      Btn b; b.icon = &qqs_i18_chevron_left; b.label = "返回";
      button(162, 180, 154, 56, b, go_wifi);
    }
  }

  static void go_scan() {
    scan_from_ap = qqs_wnet.mode == 1;
    scan_start();
    ui.goto_screen(scan_screen);
  }

  // =========================================================================
  // Module firmware: backup / restore / update the ESP8266 from the SD card (module/mks_wifi/qqs_espflash)
  // =========================================================================

  static uint32_t fw_sz[5];           // ESPSTUB.BIN, ESPBAK.BIN, MksWifi.bin, MksWifi_WebView.bin, MKS_WIFI_CUR
  static int8_t fw_arm = -1;          // button waiting for its second press
  static millis_t fw_arm_ms = 0;
  static uint8_t fw_sig = 0xFF;
  static void fwm_screen();
  static void fwp_screen();

  static char *fmt_bytes(char *o, uint32_t v) {     // "4,194,304 B"
    char t[16]; uint8_t n = 0;
    do { t[n++] = '0' + v % 10; v /= 10; } while (v);
    uint8_t k = 0;
    while (n) { o[k++] = t[--n]; if (n && n % 3 == 0) o[k++] = ','; }
    strcpy(o + k, " B");
    return o;
  }

  static void go_fwm() {
    espf_scan_sd(fw_sz);
    fw_arm = -1; fw_sig = 0xFF;
    ui.goto_screen(fwm_screen);
  }

  static uint32_t fwp_sig = 0xFFFFFFFF;
  template<uint8_t OP> static void fw_go() {
    if (job_active() || espf_busy()) return;
    if (fw_arm != OP || ELAPSED(millis(), fw_arm_ms + 3000)) { fw_arm = OP; fw_arm_ms = millis(); fw_sig = 0xFF; redraw(); return; }
    fw_arm = -1;
    espf_start(EspfOp(OP));
    fwp_sig = 0xFFFFFFFF;
    ui.goto_screen(fwp_screen);
  }

  static void fwm_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    const bool draw = live(next_ms, 500);
    if (fw_arm >= 0 && ELAPSED(millis(), fw_arm_ms + 3000)) fw_arm = -1;
    const uint8_t sig = uint8_t(fw_arm + 1);
    if (!draw || sig == fw_sig) return;
    fw_sig = sig;

    touch.clear();
    header("模組韌體", go_wifi);
    canvas(0, HEADER_H, TFT_WIDTH, 94, C_BG);
    char v[40];
    const bool nocard = fw_sz[0] == 0xFFFFFFFF, stub = !nocard && fw_sz[0];
    int16_t y = 3;
    auto row = [&](const char *label, const char *val, const uint16_t col) {
      text(6, y, F_S, C_MUTED, C_BG, label); text(66, y, F_S, col, C_BG, val); y += 18;
    };
    if (nocard) row("SD 卡", "讀不到，請確認卡有插好", C_STOP);
    else row("ESPSTUB", stub ? "有" : "沒有（四個功能都需要這個檔）", stub ? C_OK : C_STOP);
    row("備份", fw_sz[1] ? fmt_bytes(v, fw_sz[1]) : "還沒有 ESPBAK.BIN", fw_sz[1] ? C_TEXT : C_DIM);
    if (fw_sz[2]) { char b[20]; sprintf_P(v, PSTR("MksWifi.bin  %s"), fmt_bytes(b, fw_sz[2])); row("韌體", v, C_TEXT); }
    else row("韌體", fw_sz[4] ? "沒有 MksWifi.bin（上次的已改名）" : "沒有 MksWifi.bin", C_DIM);
    if (fw_sz[3]) { char b[20]; sprintf_P(v, PSTR("WebView  %s"), fmt_bytes(b, fw_sz[3])); row("網頁", v, C_TEXT); }
    else row("網頁", "沒有 MksWifi_WebView.bin", C_DIM);
    text(6, y + 1, F_S, C_WARN, C_BG, "每個按鈕都要 3 秒內按兩次。建議先備份。");

    static const char * const lab[4] = { "備份模組", "寫回備份", "更新韌體", "更新網頁" };
    static const qqs_bitmap_t * const ic[4] = { &qqs_i18_device_floppy, &qqs_i18_restore, &qqs_i18_refresh, &qqs_i18_refresh };
    static const screenFunc_t cb[4] = { fw_go<0>, fw_go<1>, fw_go<2>, fw_go<3> };
    const bool can = stub && !job_active();
    const bool en[4] = { can, can && fw_sz[1], can && fw_sz[2], can && fw_sz[3] };
    for (uint8_t i = 0; i < 4; i++) {
      Btn b; b.icon = ic[i]; b.enabled = en[i];
      if (fw_arm == i) { b.label = "再按一次確認"; b.fg = C_STOP; b.border = C_STOP; b.bg = C_STOPBG; }
      else b.label = lab[i];
      button(4 + (i & 1) * 158, 128 + (i >> 1) * 56, 154, 52, b, cb[i]);
    }
  }

  // ---- progress ----
  static void fw_retry() { if (!job_active()) { espf_start(EspfOp(espf.op)); fwp_sig = 0xFFFFFFFF; redraw(); } }

  static const char *fw_err_text(char *o) {
    switch (espf.err) {
      case EE_SD:      return "SD 卡讀不到";
      case EE_NOSTUB:  return "沒有 ESPSTUB.BIN，或檔案不對";
      case EE_NOFILE:  return "SD 卡上找不到要寫的檔案";
      case EE_SIZE:    return "檔案大小不對（太大或是空的）";
      case EE_NOTIMG:  return "MksWifi.bin 不是 ESP8266 韌體";
      case EE_FULL:    return "備份檔已有 10 個，請刪舊的";
      case EE_SYNC:    return "模組沒進燒錄模式（沒有回應）";
      case EE_OHAI:    return "stub 沒有啟動";
      case EE_BAUD:    return "三種速度都連不上";
      case EE_FLASHID: return "讀不到快閃型號";
      case EE_CMD:     sprintf_P(o, PSTR("模組回報錯誤（碼 %04X）"), espf.err_code); return o;
      case EE_TIMEOUT: return "等太久沒有回應";
      case EE_DATA:    return "收到的資料不完整";
      case EE_MD5:     return "MD5 驗證不符";
      case EE_SDWRITE: return "SD 卡讀寫失敗";
      case EE_RENAME:  return "已寫入並驗證，但檔案改名失敗";
      default:         return "";
    }
  }

  static void fwp_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    const bool draw = live(next_ms, 250);
    const uint32_t secs = (millis() - espf.stage_ms) / 1000;
    const uint8_t pct = espf.total ? uint8_t(uint64_t(espf.done) * 100 / espf.total) : 0;
    const uint32_t sig = (uint32_t(espf.running) << 31) | (uint32_t(espf.err) << 24) | (uint32_t(espf.stage) << 20)
                       | (uint32_t(pct) << 12) | _MIN(secs, 4095UL);
    if (!draw || sig == fwp_sig) return;
    fwp_sig = sig;

    static const char * const title[4] = { "備份模組", "寫回備份", "更新韌體", "更新網頁" };
    touch.clear();
    header(title[espf.op], espf.running ? nullptr : go_fwm);
    if (espf.running) {                    // no way out while the module is in its bootloader
      canvas(0, 0, 84, HEADER_H - 1, C_BAR);
    }

    // stage list (left)
    canvas(0, HEADER_H, 128, 144, C_BG);
    static const char * const st_name[EST_COUNT] = { "進燒錄模式", "載入 stub", "抹除", "寫入", "驗證", "重開" };
    int16_t y = 6;
    for (uint8_t i = 0; i < EST_COUNT; i++) {
      if (i == EST_ERASE && espf.op == ESPF_BACKUP) continue;
      const bool failed = espf.err && i == espf.stage, cur = !espf.err && espf.running && i == espf.stage,
                 done = i < espf.stage || (!espf.running && !espf.err);
      const uint16_t col = failed ? C_STOP : cur ? C_ACCENT : done ? C_OK : C_DIM;
      fill(10, y + 4, 8, 8, col);
      text(26, y, F_S, failed || cur ? col : done ? C_TEXT : C_DIM, C_BG,
           (i == EST_XFER && espf.op == ESPF_BACKUP) ? "讀取" : st_name[i]);
      y += 22;
    }

    // details (right)
    canvas(128, HEADER_H, TFT_WIDTH - 128, 144, C_BG);
    constexpr int16_t W = TFT_WIDTH - 128 - 6;
    char v[64];
    if (espf.err) {
      text(0, 6, F_L, C_STOP, C_BG, "失敗");
      sprintf_P(v, PSTR("停在「%s」"), (espf.stage == EST_XFER && espf.op == ESPF_BACKUP) ? "讀取" : st_name[espf.stage]);
      text(0, 42, F_S, C_TEXT, C_BG, v);
      text(0, 62, F_S, C_WARN, C_BG, fw_err_text(v));
      text(0, 90, F_S, C_MUTED, C_BG, espf.err == EE_RENAME ? "模組已更新好，只差改檔名。"
           : (espf.stage <= EST_STUB || espf.op == ESPF_BACKUP) ? "模組沒有被改動。" : "模組韌體可能不完整，請重試。");
    }
    else if (!espf.running) {
      text(0, 6, F_L, C_OK, C_BG, "完成");
      if (espf.op == ESPF_BACKUP) { sprintf_P(v, PSTR("已存成 %s"), espf.file); text(0, 42, F_S, C_TEXT, C_BG, v); }
      else text(0, 42, F_S, C_TEXT, C_BG, "已寫入，MD5 驗證相符");
      if (espf.reported) {
        if (mks_wifi_net_state == 1) sprintf_P(v, PSTR("模組已開機 %u.%u.%u.%u"), mks_wifi_ip[0], mks_wifi_ip[1], mks_wifi_ip[2], mks_wifi_ip[3]);
        else strcpy_P(v, PSTR("模組已開機（還沒連上網路）"));
        text(0, 62, F_S, C_OK, C_BG, v);
      }
      else text(0, 62, F_S, C_WARN, C_BG, "40 秒內沒收到模組回報");
    }
    else if (espf.stage == EST_XFER && espf.total) {
      sprintf_P(v, PSTR("%u%%"), pct);
      text(0, 2, F_L, C_TEXT, C_BG, v);
      fill(0, 36, W, 8, C_TILE);
      fill(0, 36, int16_t(uint32_t(W) * pct / 100), 8, C_ACCENT);
      sprintf_P(v, PSTR("%lu／%lu KB"), (unsigned long)(espf.done >> 10), (unsigned long)(espf.total >> 10));
      text(0, 52, F_S, C_TEXT, C_BG, v);
      if (espf.bps) { sprintf_P(v, PSTR("%lu KB/s"), (unsigned long)(espf.bps >> 10)); text(0, 70, F_S, C_MUTED, C_BG, v); }
    }
    else {
      static const char * const doing[EST_COUNT] = { "重開模組進燒錄模式…", "載入 stub、讀快閃型號…", "抹除中…", "", "請模組算 MD5…", "等模組開機回報…" };
      text(0, 10, F_S, C_TEXT, C_BG, espf.op == ESPF_BACKUP && espf.stage == EST_VERIFY ? "讀回 SD 檔案比對…" : doing[espf.stage]);
      sprintf_P(v, PSTR("%lu 秒"), (unsigned long)secs);
      text(0, 30, F_S, C_MUTED, C_BG, v);
    }

    // link (bottom of the right panel)
    if (espf.flash_size) {
      if (espf.retries) sprintf_P(v, PSTR("%lu bps　重讀 %u 次"), (unsigned long)espf.baud, espf.retries);   // drawn with F_S
      else sprintf_P(v, PSTR("%lu bps"), (unsigned long)espf.baud);
      text(0, 112, F_S, espf.retries ? C_WARN : C_DIM, C_BG, v);
      sprintf_P(v, PSTR("快閃 %lu MB（%06lX）"), (unsigned long)(espf.flash_size >> 20), (unsigned long)espf.flash_id);
      text(0, 128, F_S, C_DIM, C_BG, v);
    }
    else if (espf.running) text(0, 128, F_S, C_DIM, C_BG, "過程中請不要關機");

    if (!espf.running) {
      if (espf.err) {
        Btn r; r.icon = &qqs_i18_refresh; r.label = "重試"; r.fg = C_ACCENT; r.border = C_ACCENT; r.enabled = !job_active();
        button(4, 184, 154, 52, r, fw_retry);
        Btn k; k.icon = &qqs_i18_chevron_left; k.label = "返回";
        button(162, 184, 154, 52, k, go_fwm);
      }
      else {
        Btn k; k.icon = &qqs_i18_check; k.label = "完成"; k.fg = C_ACCENT; k.border = C_ACCENT;
        button(83, 184, 154, 52, k, go_fwm);
      }
    }
    else canvas(0, 184, TFT_WIDTH, 52, C_BG);
  }

#endif // MKS_WIFI

} // namespace qqs

#endif // QQS_UI
