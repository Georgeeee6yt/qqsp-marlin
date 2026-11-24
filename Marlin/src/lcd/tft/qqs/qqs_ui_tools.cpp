/**
 * QQS-Pro v3.2 UI — Z offset, end-of-print babystep prompt, sensor page + endstop repeatability,
 * manual delta calibration wizard. The motion side is in qqs_tools.cpp (M2003 / M2005 / M2006).
 *
 * Every Chinese string here is collected by qqs_tools/gen_assets.py (re-run it after text changes).
 * Lines that draw with F_L must keep that token on the same line as the literal.
 */
#include "../../../inc/MarlinConfig.h"

#if ENABLED(QQS_UI)

#include "qqs_ui_priv.h"
#include "qqs_tools.h"
#include "qqs_settings.h"
#include "../tft.h"
#include "../touch.h"
#include "../../../module/temperature.h"
#include "../../../module/planner.h"
#include "../../../module/motion.h"
#include "../../../module/delta.h"
#include "../../../module/settings.h"
#include "../../../gcode/queue.h"
#include "qqs_wifi_diag.h"
#ifdef MKS_WIFI
  extern uint8_t mks_wifi_ip[4];
  extern uint8_t mks_wifi_net_state;
#endif
#if HAS_FILAMENT_SENSOR
  #include "../../../feature/runout.h"
#endif
#if HAS_BUZZER
  #include "../../../libs/buzzer.h"
#endif

namespace qqs {

  static bool queue_idle() { return !queue.has_commands_queued() && !planner.has_blocks_queued(); }
  static bool inject_free() { return !queue.injected_commands_P && !queue.injected_commands[0]; }

  // "0.013" / "-0.025"
  static char *f3(char *o, const float v) {
    long h = lroundf(v * 1000.0f);
    const bool neg = h < 0;
    if (neg) h = -h;
    sprintf_P(o, neg ? PSTR("-%ld.%03ld") : PSTR("%ld.%03ld"), h / 1000, h % 1000);
    return o;
  }
  // "+0.06" / "-0.06"
  static char *s2(char *o, const float v) {
    char t[16]; f2(t, ABS(v));
    sprintf_P(o, PSTR("%c%s"), v < 0 ? '-' : '+', t);
    return o;
  }

  // =========================================================================
  // Z offset (first-layer offset)
  // =========================================================================

  static float zo_pending = 0;    // taps not yet sent as M2003

  static void zo_flush(const bool save = false) {
    if (!inject_free()) return;
    char cmd[40], a[16];
    if (ABS(zo_pending) > 0.0001f) {
      sprintf_P(cmd, save ? PSTR("M2003 D%s S") : PSTR("M2003 D%s"), f2(a, zo_pending));
      queue.inject(cmd);
      zo_pending = 0;
    }
    else if (save) queue.inject(F("M500"));
  }
  static void zo_add(const float d) { zo_pending += d; zo_flush(); redraw(); }
  static void zo_m10() { zo_add(-0.10f); }
  static void zo_m05() { zo_add(-0.05f); }
  static void zo_m01() { zo_add(-0.01f); }
  static void zo_p01() { zo_add(+0.01f); }
  static void zo_p05() { zo_add(+0.05f); }
  static void zo_p10() { zo_add(+0.10f); }
  static void zo_center() {
    if (run(all_axes_homed() ? "G1 Z5 F3000\nG1 X0 Y0 F4800\nG1 Z0 F300" : "G28\nG1 Z5 F3000\nG1 X0 Y0 F4800\nG1 Z0 F300"))
      ui.set_status("到中心 Z0：紙放噴嘴下試拉");
    redraw();
  }
  static void zo_lift() { if (run("G1 Z10 F600")) ui.set_status("抬到 Z10"); redraw(); }
  static void zo_save() {
    if (!inject_free()) { ui.set_status("忙碌中，請稍候"); redraw(); return; }
    zo_flush(true);
    ui.set_status("第一層偏移已存檔");
    redraw();
  }

  static void zoff_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    zo_flush();
    if (!live(next_ms, 500)) return;

    touch.clear();
    header("Z 偏移", go_settings);
    canvas(0, HEADER_H, TFT_WIDTH, 92, C_BG);
    char v[24], a[16];
    const float shown = qqs_settings.layer_offset + zo_pending;
    text_c(0, 0, TFT_WIDTH, F_XL, shown == 0 ? C_TEXT : C_ACCENT, C_BG, s2(v, shown));
    text_c(0, 48, TFT_WIDTH, F_S, C_MUTED, C_BG, "第一層偏移（mm）　正值＝噴頭抬高、第一層變厚");
    sprintf_P(v, PSTR("高度 H %s"), f2(a, delta_height - zo_pending));
    text_c(0, 68, TFT_WIDTH, F_S, C_DIM, C_BG, v);

    static const char * const lab[6] = { "−0.1", "−0.05", "−0.01", "+0.01", "+0.05", "+0.1" };
    static const screenFunc_t cb[6] = { zo_m10, zo_m05, zo_m01, zo_p01, zo_p05, zo_p10 };
    for (uint8_t i = 0; i < 6; i++) { Btn b; b.label = lab[i]; button(4 + i * 53, 128, 49, 48, b, cb[i]); }

    { Btn b; b.icon = &qqs_i18_focus_centered; b.label = "到中心 Z0"; b.enabled = !job_active(); button(4, 182, 120, 54, b, zo_center); }
    { Btn b; b.icon = &qqs_i18_arrow_bar_up;   b.label = "抬起";      b.enabled = !job_active(); button(128, 182, 84, 54, b, zo_lift); }
    { Btn b; b.icon = &qqs_i18_device_floppy;  b.label = "存檔"; b.fg = C_OK; b.border = C_OK; button(216, 182, 100, 54, b, zo_save); }
  }

  void go_zoff() {
    if (job_active()) { ui.set_status("列印中請用列印畫面的 Z 微調"); redraw(); return; }
    zo_pending = 0;
    ui.goto_screen(zoff_screen);
  }

  // =========================================================================
  // End of a print with Z babystepping: keep it?
  // =========================================================================

  static float bsp_total = 0;

  static void bsp_no()  { ui.return_to_status(); }
  static void bsp_yes() {
    char cmd[40], a[16];
    sprintf_P(cmd, PSTR("M2003 D%s S N"), f2(a, bsp_total));
    if (!run(cmd)) return;
    char v[40];
    sprintf_P(v, PSTR("已存：第一層偏移 %s"), s2(a, qqs_settings.layer_offset + bsp_total));
    ui.set_status(v);
    ui.return_to_status();
  }

  static void bs_prompt_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 60000)) return;
    touch.clear();
    tft.fill(0, 0, TFT_WIDTH, TFT_HEIGHT, C_BG);   // entered from the status screen without a clear
    header("Z 微調", bsp_no);
    canvas(0, HEADER_H, TFT_WIDTH, 116, C_BG);
    char v[48], a[16], b[16];
    sprintf_P(v, PSTR("這次 Z 微調 %s"), s2(a, bsp_total));   // drawn with F_L
    text_c(0, 10, TFT_WIDTH, F_L, C_TEXT, C_BG, v);
    text_c(0, 48, TFT_WIDTH, F_M, C_TEXT, C_BG, "要存成之後的預設嗎？");
    sprintf_P(v, PSTR("第一層偏移 %s → %s"), s2(a, qqs_settings.layer_offset), s2(b, qqs_settings.layer_offset + bsp_total));
    text_c(0, 76, TFT_WIDTH, F_S, C_MUTED, C_BG, v);
    text_c(0, 94, TFT_WIDTH, F_S, C_DIM, C_BG, "不存的話，下次列印還是原本的高度。");
    Btn n; n.icon = &qqs_i28_x; n.label = "不用";
    button(4, 154, 154, 82, n, bsp_no);
    Btn y; y.icon = &qqs_i28_device_floppy; y.label = "存"; y.fg = C_OK; y.border = C_OK;
    button(162, 154, 154, 82, y, bsp_yes);
  }

  void open_bs_prompt(const float total) {
    bsp_total = total;
    ui.goto_screen(bs_prompt_screen);
  }

  // =========================================================================
  // Sensor page (second tab of machine info)
  // =========================================================================

  static void sensor_screen();
  static void rep_screen();

  void info_tabs(const uint8_t tab) {
    canvas(0, HEADER_H, TFT_WIDTH, 32, C_BG);
    static const char * const lab[3] = { "機器資訊", "感測器", "Wi-Fi" };
    static const screenFunc_t cb[3] = { go_info, go_sensors, go_wifi };
    for (uint8_t i = 0; i < 3; i++) {
      Btn b; b.label = lab[i];
      if (i == tab) { b.fg = C_ACCENT; b.border = C_ACCENT; }
      button(4 + i * 104, HEADER_H + 2, 100, 28, b, cb[i]);
    }
  }

  static uint16_t sn_count[4];
  static bool sn_state[4];

  static bool sn_read(const uint8_t i) {
    switch (i) {
      case 0: return READ(X_MAX_PIN) != X_MAX_ENDSTOP_INVERTING;
      case 1: return READ(Y_MAX_PIN) != Y_MAX_ENDSTOP_INVERTING;
      case 2: return READ(Z_MAX_PIN) != Z_MAX_ENDSTOP_INVERTING;
      default: return READ(Z_MIN_PIN) != Z_MIN_ENDSTOP_INVERTING;
    }
  }

  // Called every idle: count every untriggered -> triggered edge (bounces included, that's the point).
  void sensor_poll() {
    if (ui.currentScreen != sensor_screen) return;
    bool changed = false;
    for (uint8_t i = 0; i < 4; i++) {
      const bool s = sn_read(i);
      if (s != sn_state[i]) {
        sn_state[i] = s;
        changed = true;
        if (s) {
          sn_count[i]++;
          #if HAS_BUZZER
            static millis_t beep_ms = 0;
            if (ELAPSED(millis(), beep_ms)) { BUZZ(25, 2400); beep_ms = millis() + 120; }
          #endif
        }
      }
    }
    if (changed) redraw();
  }

  static void sn_clear() { for (uint8_t i = 0; i < 4; i++) sn_count[i] = 0; redraw(); }
  static void sn_probe_toggle() { qqs_settings.probe_installed ^= 1; ui.set_status("要保留請到設定頁按存檔"); redraw(); }
  static void sn_rep() { rep.done = false; ui.goto_screen(rep_screen); }

  static void sensor_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 1000)) return;

    touch.clear();
    header("機器資訊", go_settings);
    info_tabs(1);

    static const char * const name[4] = { "X 塔", "Y 塔", "Z 塔", "探頭" };
    for (uint8_t i = 0; i < 4; i++) {
      const int16_t x = 4 + i * 79, y = HEADER_H + 36, w = 75, h = 84;
      const bool probe = i == 3, absent = probe && !qqs_settings.probe_installed, on = sn_state[i];
      const uint16_t bg = on && !absent ? QRGB(0x1C, 0x3A, 0x2E) : C_TILE;
      canvas(x, y, w, h, bg);
      if (absent) {   // dashed frame
        for (int16_t k = 0; k < w; k += 6) { fill(k, 0, 3, 1, C_DIM); fill(k, h - 1, 3, 1, C_DIM); }
        for (int16_t k = 0; k < h; k += 6) { fill(0, k, 1, 3, C_DIM); fill(w - 1, k, 1, 3, C_DIM); }
      }
      else frame(0, 0, w, h, on ? C_OK : C_BORDER);
      text_c(0, 4, w, F_M, absent ? C_MUTED : C_TEXT, bg, name[i]);
      if (absent) {
        text_c(0, 30, w, F_S, C_DIM, bg, "未安裝");
        text_c(0, 48, w, F_S, C_DIM, bg, on ? "（觸發）" : "（未觸發）");
        text_c(0, 64, w, F_S, C_DIM, bg, "點一下切換");
      }
      else {
        text_c(0, 30, w, F_M, on ? C_OK : C_MUTED, bg, on ? "觸發" : "未觸發");
        char c[16]; sprintf_P(c, PSTR("次數 %u"), sn_count[i]);
        text_c(0, 60, w, F_S, C_MUTED, bg, c);
      }
      if (probe) touch.add_control(BUTTON, x, y, w, h, (intptr_t)sn_probe_toggle);
    }

    canvas(0, HEADER_H + 122, TFT_WIDTH, 22, C_BG);
    text_c(0, 2, TFT_WIDTH, F_S, C_MUTED, C_BG, "馬達已放鬆，可手推滑車。推一次跳很多次＝彈跳。");

    Btn c; c.icon = &qqs_i18_restore; c.label = "次數歸零";
    button(4, 180, 154, 56, c, sn_clear);
    Btn t; t.icon = &qqs_i18_chart_line; t.label = "一致性測試"; t.fg = C_ACCENT; t.border = C_ACCENT;
    button(162, 180, 154, 56, t, sn_rep);
  }

  void go_sensors() {
    if (job_active()) { ui.set_status("列印中不能開感測器頁"); redraw(); return; }
    if (!run("M84")) return;           // steppers off so the carriages can be pushed by hand
    set_all_unhomed();                 // and they must be homed again before moving
    for (uint8_t i = 0; i < 4; i++) { sn_count[i] = 0; sn_state[i] = sn_read(i); }
    ui.goto_screen(sensor_screen);
  }


  // =========================================================================
  // Wi-Fi diagnostics (third tab of machine info)
  // =========================================================================

  static void wifi_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 1000)) return;

    touch.clear();
    header("機器資訊", go_settings);
    info_tabs(2);
    canvas(0, HEADER_H + 32, TFT_WIDTH, TFT_HEIGHT - HEADER_H - 32, C_BG);
    const qqs_wifi_diag_t &d = qqs_wdiag;
    char v[64];
    int16_t y = 2;
    auto row = [&](const char *label, const char *val, const uint16_t col = C_TEXT) {
      text(6, y, F_S, C_MUTED, C_BG, label);
      text(70, y, F_S, col, C_BG, val);
      y += 17;
    };

    #ifdef MKS_WIFI
      if (mks_wifi_net_state == 1) sprintf_P(v, PSTR("已連線 %u.%u.%u.%u"), mks_wifi_ip[0], mks_wifi_ip[1], mks_wifi_ip[2], mks_wifi_ip[3]);
      else strcpy_P(v, mks_wifi_net_state == 2 ? PSTR("異常") : (mks_wifi_net_state == 3 ? PSTR("未設定") : PSTR("沒收到狀態")));
      row("連線", v, mks_wifi_net_state == 1 ? C_OK : C_WARN);
    #endif
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

  void go_wifi() { ui.goto_screen(wifi_screen); }

  // ---- Endstop repeatability ----

  static void rep_n5()   { qqs_settings.test_count = 5;  redraw(); }
  static void rep_n10()  { qqs_settings.test_count = 10; redraw(); }
  static void rep_start(){ if (run("M2005")) { rep.running = true; rep.done = false; rep.tower = 0; rep.sample = 0; } redraw(); }
  static void rep_stop() { rep.abort = true; redraw(); }
  static void rep_back() { if (rep.running) return; go_sensors(); }

  static void rep_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 500)) return;

    touch.clear();
    header("限位一致性測試", rep_back);
    canvas(0, HEADER_H, TFT_WIDTH, 144, C_BG);
    char v[64], a[16];

    if (!rep.running && !rep.done) {
      text(10, 6,  F_S, C_TEXT, C_BG, "先歸零。每個塔：滑車往下 5 mm，再以 4.5 mm/s");
      text(10, 24, F_S, C_TEXT, C_BG, "（歸零最後一段的速度）往上碰開關，重複幾次。");
      text(10, 50, F_S, C_MUTED, C_BG, "≤ 0.02 綠　0.02–0.04 黃　> 0.04 紅");
      text(10, 68, F_S, C_MUTED, C_BG, "同方向一直偏＝固定鬆動；忽高忽低＝開關或彈跳。");
      text(10, 98, F_M, C_TEXT, C_BG, "每塔次數");
      Btn b5;  b5.label = "5 次";   if (qqs_settings.test_count != 10) { b5.fg = C_ACCENT; b5.border = C_ACCENT; }
      button(110, HEADER_H + 90, 90, 40, b5, rep_n5);
      Btn b10; b10.label = "10 次"; if (qqs_settings.test_count == 10) { b10.fg = C_ACCENT; b10.border = C_ACCENT; }
      button(206, HEADER_H + 90, 90, 40, b10, rep_n10);
      Btn s; s.icon = &qqs_i28_player_play; s.label = "開始"; s.fg = C_ACCENT; s.border = C_ACCENT;
      button(4, 180, 312, 56, s, rep_start);
      return;
    }

    // Results (partial while running)
    static const char * const tn[3] = { "X 塔", "Y 塔", "Z 塔" };
    for (uint8_t t = 0; t < 3; t++) {
      const int16_t y = 6 + t * 44;
      text(10, y, F_M, C_TEXT, C_BG, tn[t]);
      const bool have = rep.done || (rep.running && (t < rep.tower));
      if (!have) {
        if (rep.running && t == rep.tower) {
          sprintf_P(v, PSTR("測試中… 第 %u/%u 次"), _MAX(rep.sample, uint8_t(1)), rep.n);
          text(70, y + 2, F_S, C_WARN, C_BG, v);
        }
        else text(70, y + 2, F_S, C_DIM, C_BG, "等待中");
        continue;
      }
      const float r = rep_range(t), d = rep_drift(t);
      const uint16_t col = r <= 0.02f ? C_OK : (r <= 0.04f ? C_WARN : C_STOP);
      sprintf_P(v, PSTR("範圍 %s mm"), f3(a, r));
      text(70, y, F_M, col, C_BG, v);
      const char *diag = r <= 0.02f ? "穩定" : (ABS(d) > 0.6f * r ? "一直往同方向偏：檢查固定、皮帶" : "忽高忽低：開關品質或彈跳");
      text(70, y + 22, F_S, r <= 0.02f ? C_MUTED : col, C_BG, diag);
    }

    if (rep.running) {
      Btn s; s.icon = &qqs_i28_player_stop; s.label = rep.abort ? "停止中…" : "停止"; s.fg = C_STOP; s.border = C_STOP; s.enabled = !rep.abort;
      button(4, 180, 312, 56, s, rep_stop);
    }
    else {
      Btn again; again.icon = &qqs_i28_restore; again.label = "再測一次"; button(4, 180, 154, 56, again, rep_start);
      Btn back;  back.icon = &qqs_i28_check;    back.label = "完成";    button(162, 180, 154, 56, back, rep_back);
    }
  }

  // =========================================================================
  // Manual delta calibration
  // =========================================================================

  static const uint8_t cal_order4[4] = { 0, 1, 2, 3 };
  static const uint8_t cal_order7[7] = { 0, 1, 4, 2, 5, 3, 6 };
  static const char * const cal_name[CAL_MAX_PTS] = { "中心", "X 塔", "Y 塔", "Z 塔", "X–Y 之間", "Y–Z 之間", "Z–X 之間" };

  static uint8_t cal_n = 4, cal_idx = 0, cal_round = 0, cal_fail = 0;
  static float cal_z[CAL_MAX_PTS], cal_cur = 1.5f, cal_prev = NAN;
  static bool cal_ok = false;
  static millis_t cal_hot_ms = 0, cal_confirm_ms = 0;
  static float cal_orig_H, cal_orig_adj[3], cal_orig_R;   // to undo "apply and measure again"

  static uint8_t cal_pt() { return (cal_n == 7 ? cal_order7 : cal_order4)[cal_idx]; }

  static void cal_prep_screen();
  static void cal_point_screen();
  static void cal_result_screen();

  static void cal_goto_point(const uint8_t i, const float z) {
    char cmd[40], a[16];
    sprintf_P(cmd, PSTR("M2006 P%u Z%s"), i, f2(a, z));
    if (run(cmd)) cal_cur = z;
  }

  // Heater targets from before the wizard raised them to 140/70: put back when leaving (0 = off).
  static int16_t cal_noz0 = 0, cal_bed0 = 0;
  static char *cal_heat_back(char *o) { sprintf_P(o, PSTR("M104 S%d\nM140 S%d"), cal_noz0, cal_bed0); return o; }

  // Leave the wizard: restore the starting values (and the heaters).
  static void cal_leave() {
    cal_res.H = cal_orig_H; cal_res.R = cal_orig_R;
    LOOP_ABC(i) cal_res.adj[i] = cal_orig_adj[i];
    char h[32], cmd[64];
    sprintf_P(cmd, cal_active ? PSTR("M2006 A\nM2006 S0\n%s") : PSTR("M2006 S0\n%s"), cal_heat_back(h));
    if (!run(cmd)) return;
    ui.return_to_status();
  }
  // Back from the prep page (nothing started): heaters back, to the settings page.
  static void cal_prep_back() {
    char h[32];
    if (!run(cal_heat_back(h))) return;
    go_settings();
  }

  // ---- Prep ----

  static void cal_mode4() { cal_n = 4; redraw(); }
  static void cal_mode7() { cal_n = 7; redraw(); }

  static bool cal_hot() {
    return thermalManager.degTargetHotend(0) >= 140 && thermalManager.degTargetBed() >= 70
        && thermalManager.wholeDegHotend(0) >= 138 && thermalManager.wholeDegBed() >= 69;
  }
  static uint16_t cal_soak_left() {   // seconds
    if (!cal_hot_ms) return qqs_settings.soak_minutes * 60;
    const millis_t need = millis_t(qqs_settings.soak_minutes) * 60000UL, el = millis() - cal_hot_ms;
    return el >= need ? 0 : (need - el + 999) / 1000;
  }

  static void cal_start() {
    if (job_active() || !cal_hot() || cal_soak_left()) return;
    cal_orig_H = delta_height + qqs_settings.layer_offset;   // in the calibration frame
    LOOP_ABC(i) cal_orig_adj[i] = delta_endstop_adj[i];
    cal_orig_R = delta_radius;
    cal_round = 1; cal_fail = 0; cal_idx = 0; cal_prev = NAN;
    char cmd[48];
    sprintf_P(cmd, PSTR("M2006 S1\nM2006 P%u Z1.50"), cal_pt());
    if (!run(cmd)) return;
    cal_cur = 1.5f;
    ui.set_status("歸零後到第一點…");
    ui.goto_screen(cal_point_screen);
  }

  static void cal_prep_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (cal_hot()) { if (!cal_hot_ms) cal_hot_ms = millis(); } else cal_hot_ms = 0;
    if (!live(next_ms, 1000)) return;

    touch.clear();
    header("手動校準", cal_prep_back);
    canvas(0, HEADER_H, TFT_WIDTH, 100, C_BG);
    text(10, 4,  F_S, C_TEXT, C_BG, "1. 清乾淨噴嘴，準備一張影印紙。");
    text(10, 22, F_S, C_TEXT, C_BG, "2. 到溫後按「開始」：先歸零，再一點一點量。");
    text(10, 40, F_S, C_TEXT, C_BG, "3. 每點把紙放噴嘴下，調到拉紙有一點阻力。");
    char v[48];
    const bool hot = cal_hot();
    const uint16_t soak = cal_soak_left();
    if (!hot) sprintf_P(v, PSTR("加熱中　噴頭 %d°/140°　熱床 %d°/70°"), thermalManager.wholeDegHotend(0), thermalManager.wholeDegBed());
    else if (soak) sprintf_P(v, PSTR("到溫了，穩定等待 %u:%02u"), soak / 60, soak % 60);
    else strcpy(v, "到溫了，可以開始");
    text(10, 68, F_M, hot && !soak ? C_OK : C_WARN, C_BG, v);

    Btn m4; m4.label = "4 點"; m4.sub = "中心＋三塔"; if (cal_n == 4) { m4.fg = C_ACCENT; m4.border = C_ACCENT; }
    button(4, 136, 154, 44, m4, cal_mode4);
    Btn m7; m7.label = "7 點"; m7.sub = "再加三塔之間"; if (cal_n == 7) { m7.fg = C_ACCENT; m7.border = C_ACCENT; }
    button(162, 136, 154, 44, m7, cal_mode7);
    Btn s; s.icon = &qqs_i18_player_play; s.label = "開始"; s.fg = C_ACCENT; s.border = C_ACCENT; s.enabled = hot && !soak && !job_active();
    button(4, 186, 312, 50, s, cal_start);
  }

  void go_cal() {
    if (job_active()) { ui.set_status("列印中不能校準"); redraw(); return; }
    cal_noz0 = thermalManager.degTargetHotend(0);
    cal_bed0 = thermalManager.degTargetBed();
    if (cal_noz0 < 140) thermalManager.setTargetHotend(140, 0);
    if (cal_bed0 < 70) thermalManager.setTargetBed(70);
    cal_hot_ms = 0;
    ui.goto_screen(cal_prep_screen);
  }

  // ---- Point ----

  static void cal_adj(const float d) {
    const float z = constrain(cal_cur + d, -2.0f, 10.0f);
    char cmd[32], a[16];
    sprintf_P(cmd, PSTR("M2006 Z%s"), f2(a, z));
    if (run(cmd)) cal_cur = z;
    redraw();
  }
  static void cal_m50() { cal_adj(-0.5f); }
  static void cal_m10() { cal_adj(-0.1f); }
  static void cal_m02() { cal_adj(-0.02f); }
  static void cal_p02() { cal_adj(+0.02f); }
  static void cal_p10() { cal_adj(+0.1f); }
  static void cal_p50() { cal_adj(+0.5f); }

  static void cal_prev_pt() {
    if (!cal_idx || !queue_idle()) return;
    cal_idx--;
    cal_prev = NAN;
    cal_goto_point(cal_pt(), cal_z[cal_pt()] + 0.5f);   // come back a little above the last value
    redraw();
  }
  static void cal_rehome() {
    if (!queue_idle()) return;
    char cmd[40], a[16];
    sprintf_P(cmd, PSTR("M2006 R%u Z%s"), cal_pt(), f2(a, cal_cur));
    if (run(cmd)) { cal_prev = cal_cur; ui.set_status("重新歸零後回到同一點…"); }
    redraw();
  }
  static void cal_point_done() {
    if (!queue_idle()) return;
    cal_z[cal_pt()] = cal_cur;
    cal_prev = NAN;
    if (++cal_idx < cal_n) {
      cal_goto_point(cal_pt(), 1.5f);
      redraw();
      return;
    }
    // All points measured
    cal_idx = cal_n - 1;
    float zz[CAL_MAX_PTS];
    for (uint8_t i = 0; i < cal_n; i++) zz[i] = cal_z[i];
    cal_ok = cal_solve(zz, cal_n);
    float worst = 0;
    for (uint8_t i = 0; i < cal_n; i++) NOLESS(worst, ABS(cal_z[i]));
    if (worst > 0.03f) cal_fail++; else cal_fail = 0;
    run("M2006 Z5");
    cal_confirm_ms = 0;
    ui.goto_screen(cal_result_screen);
  }
  static void cal_point_back() {
    // Two taps within 3 s to give up (a phantom touch shouldn't throw the measurements away)
    if (cal_confirm_ms && PENDING(millis(), cal_confirm_ms + 3000)) { cal_confirm_ms = 0; cal_leave(); return; }
    cal_confirm_ms = millis();
    ui.set_status("再按一次「返回」就放棄這次校準");
    redraw();
  }

  static void cal_point_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 400)) return;

    touch.clear();
    char t[40], v[48], a[16], b[16];
    sprintf_P(t, PSTR("第 %u/%u 點：%s"), cal_idx + 1, cal_n, cal_name[cal_pt()]);
    header(t, cal_point_back);

    const bool busy = !queue_idle();
    canvas(0, HEADER_H, TFT_WIDTH, 88, C_BG);
    text_c(0, 0, TFT_WIDTH, F_XL, C_TEXT, C_BG, f2(a, cal_cur));
    if (busy) text_c(0, 48, TFT_WIDTH, F_S, C_WARN, C_BG, "移動中…");
    else      text_c(0, 48, TFT_WIDTH, F_S, C_MUTED, C_BG, "噴嘴高度 Z：調到拉紙有一點阻力");
    if (!isnan(cal_prev) && qqs_settings.show_remeasure_diff) {
      sprintf_P(v, PSTR("重新歸零前 %s　這點兩次相差 %s"), f2(a, cal_prev), s2(b, cal_cur - cal_prev));
      text_c(0, 68, TFT_WIDTH, F_S, ABS(cal_cur - cal_prev) > 0.03f ? C_WARN : C_MUTED, C_BG, v);
    }
    else {
      sprintf_P(v, PSTR("第 %u 輪"), cal_round);
      text_c(0, 68, TFT_WIDTH, F_S, C_DIM, C_BG, v);
    }

    static const char * const lab[6] = { "−0.5", "−0.1", "−0.02", "+0.02", "+0.1", "+0.5" };
    static const screenFunc_t cb[6] = { cal_m50, cal_m10, cal_m02, cal_p02, cal_p10, cal_p50 };
    for (uint8_t i = 0; i < 6; i++) { Btn b_; b_.label = lab[i]; button(4 + i * 53, 124, 49, 50, b_, cb[i]); }

    Btn p; p.label = "上一點"; p.enabled = cal_idx > 0 && !busy; button(4, 180, 84, 56, p, cal_prev_pt);
    Btn r; r.label = "重新歸零"; r.sub = "再量一次"; r.enabled = !busy; button(92, 180, 108, 56, r, cal_rehome);
    Btn d; d.icon = &qqs_i18_check; d.label = "這點好了"; d.fg = C_OK; d.border = C_OK; d.enabled = !busy; button(204, 180, 112, 56, d, cal_point_done);
  }

  // ---- Result ----

  static void cal_abandon() { cal_leave(); }
  static void cal_again() {
    if (!cal_ok) return;
    char cmd[48];
    cal_idx = 0; cal_prev = NAN; cal_round++;
    sprintf_P(cmd, PSTR("M2006 A\nM2006 P%u Z1.50"), cal_pt());
    if (!run(cmd)) return;
    cal_cur = 1.5f;
    ui.set_status("套用後歸零，再量一輪…");
    ui.goto_screen(cal_point_screen);
  }
  static void cal_save() {
    if (!cal_ok) return;
    float worst = 0;
    for (uint8_t i = 0; i < cal_n; i++) NOLESS(worst, ABS(cal_z[i]));
    if (worst > 0.03f) {
      if (cal_fail < 5) return;
      // After 5 failed rounds: allowed, but only with a second tap within 3 s
      if (!cal_confirm_ms || ELAPSED(millis(), cal_confirm_ms + 3000)) { cal_confirm_ms = millis(); ui.set_status("3 秒內再按一次就強制存檔"); redraw(); return; }
    }
    char h[32], cmd[64];
    sprintf_P(cmd, PSTR("M2006 A\nM2006 S0\nM500\n%s"), cal_heat_back(h));
    if (!run(cmd)) return;
    ui.set_status("校準值已套用並存檔");
    ui.return_to_status();
  }

  static void cal_result_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 1000)) return;

    touch.clear();
    header("校準結果", cal_point_back);   // two taps to give up
    canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
    char v[64], a[16], b[16];

    float worst = 0;
    for (uint8_t i = 0; i < cal_n; i++) NOLESS(worst, ABS(cal_z[i]));
    const bool pass = worst <= 0.03f;

    // Measured points, two columns
    for (uint8_t k = 0; k < cal_n; k++) {
      const uint8_t i = (cal_n == 7 ? cal_order7 : cal_order4)[k];
      const int16_t x = (k & 1) ? 164 : 8, y = 2 + (k / 2) * 17;
      const float e = ABS(cal_z[i]);
      sprintf_P(v, PSTR("%s %s"), cal_name[i], s2(a, cal_z[i]));
      text(x, y, F_S, e <= 0.03f ? C_OK : (e <= 0.06f ? C_WARN : C_STOP), C_BG, v);
    }
    int16_t y = 2 + ((cal_n + 1) / 2) * 17 + 2;
    sprintf_P(v, PSTR("最大誤差 %s mm%s"), f2(a, worst), pass ? "（合格）" : "（超過 0.03）");
    text(8, y, F_M, pass ? C_OK : C_WARN, C_BG, v);
    y += 24;

    if (!cal_ok) {
      text(8, y, F_S, C_STOP, C_BG, "算不出合理的值：量測可能有誤，請放棄後重來。");
    }
    else {
      sprintf_P(v, PSTR("H %s → %s"), f2(a, delta_height), f2(b, cal_res.H));
      text(8, y, F_S, C_TEXT, C_BG, v);
      sprintf_P(v, PSTR("R %s → %s"), f2(a, delta_radius), f2(b, cal_res.R));
      text(164, y, F_S, C_TEXT, C_BG, v);
      y += 17;
      char d[16];
      sprintf_P(v, PSTR("塔柱偏移 X%s Y%s Z%s"), f2(a, cal_res.adj[0]), f2(b, cal_res.adj[1]), f2(d, cal_res.adj[2]));
      text(8, y, F_S, C_TEXT, C_BG, v);
      y += 17;
      if (cal_n == 7) {
        sprintf_P(v, PSTR("剩餘誤差 %s%s"), f3(a, cal_res.resid_max), cal_res.resid_max > 0.05f ? "：可能是塔柱角度或熱床不平" : "");
        text(8, y, F_S, cal_res.resid_max > 0.05f ? C_WARN : C_MUTED, C_BG, v);
      }
      else if (cal_fail >= 5) text(8, y, F_S, C_WARN, C_BG, "連續 5 輪過不了：建議先做限位一致性測試。");
    }

    Btn ab; ab.icon = &qqs_i18_x; ab.label = "放棄"; button(4, 180, 84, 56, ab, cal_abandon);
    Btn ag; ag.label = "套用"; ag.sub = "再量一輪"; ag.enabled = cal_ok; button(92, 180, 108, 56, ag, cal_again);
    Btn sv; sv.icon = &qqs_i18_device_floppy; sv.label = (!pass && cal_fail >= 5) ? "強制存檔" : "套用並存檔";
    sv.enabled = cal_ok && (pass || cal_fail >= 5);
    if (sv.enabled) { sv.fg = pass ? C_OK : C_STOP; sv.border = sv.fg; }
    button(204, 180, 112, 56, sv, cal_save);
  }


  // =========================================================================
  // Shared: save button in the header, on/off pill
  // =========================================================================

  // Settings sub-pages have no status line, so a save is confirmed on the button itself:
  // green "已存" (or red "失敗") for 2 s plus a beep.
  static millis_t save_fb_ms = 0;
  static bool save_fb_ok = true;
  void save_feedback(const bool ok) {
    save_fb_ms = millis(); save_fb_ok = ok;
    #if HAS_BUZZER
      if (ok) BUZZ(40, 2400); else { BUZZ(150, 600); }
    #endif
    redraw();
  }
  void save_feedback_poll() { if (save_fb_ms && ELAPSED(millis(), save_fb_ms + 2000)) { save_fb_ms = 0; redraw(); } }
  void save_button(const int16_t x, const int16_t y, const int16_t w, const int16_t h, screenFunc_t cb, const bool enabled) {
    Btn b; b.bg = C_BAR; b.enabled = enabled;
    if (save_fb_ms) { b.label = save_fb_ok ? "已存" : "失敗"; b.fg = b.border = save_fb_ok ? C_OK : C_STOP; }
    else            { b.label = "存檔"; b.fg = b.border = C_ACCENT; }
    button(x, y, w, h, b, cb);
  }

  // M500 through the queue, so it runs after anything just injected (M92, M412, ...)
  static void hdr_save() { save_feedback(run("M500")); }
  static void header_save(const char *title, screenFunc_t back, const bool can_save = true) {
    header(title, back);
    save_button(258, 3, 58, HEADER_H - 7, hdr_save, can_save);
  }
  static void pill(const int16_t x, const int16_t y, const bool on, screenFunc_t cb) {
    Btn b; b.label = on ? "開" : "關";
    if (on) { b.fg = C_OK; b.border = C_OK; }
    button(x, y, 64, 32, b, cb);
  }

  // =========================================================================
  // PID
  // =========================================================================

  static uint8_t pid_state = 0;     // 0 idle, 1 hotend running, 2 bed running, 3 done
  static millis_t pid_ms = 0;

  static void pid_start(const bool bed) {
    if (job_active() || pid_state == 1 || pid_state == 2) return;
    if (!run(bed ? "M303 E-1 S60 C8 U1" : "M303 E0 S220 C8 U1")) return;
    pid_state = bed ? 2 : 1;
    pid_ms = millis();
    ui.set_status(bed ? "熱床 PID 調校中（約 5–10 分鐘）" : "噴頭 PID 調校中（約 3–5 分鐘）");
    redraw();
  }
  static void pid_hot() { pid_start(false); }
  static void pid_bed() { pid_start(true); }
  static void pid_stop() { wait_for_heatup = false; ui.set_status("已停止調校"); redraw(); }

  static void pid_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    save_feedback_poll();
    if ((pid_state == 1 || pid_state == 2) && ELAPSED(millis(), pid_ms + 2000) && queue_idle()) {
      pid_state = 3;
      #if HAS_BUZZER
        BUZZ(200, 2000);
      #endif
      redraw();
    }
    if (!live(next_ms, 1000)) return;

    touch.clear();
    header_save("PID 調整", go_settings, !(pid_state == 1 || pid_state == 2));
    canvas(0, HEADER_H, TFT_WIDTH, 140, C_BG);
    char v[64], a[16], b[16], c[16];
    #if ENABLED(PIDTEMP)
      const auto &hp = thermalManager.temp_hotend[0].pid;
      text(10, 6, F_M, C_TEXT, C_BG, "噴頭");
      sprintf_P(v, PSTR("P %s  I %s  D %s"), f2(a, hp.p()), f2(b, hp.i()), f2(c, hp.d()));
      text(60, 8, F_S, C_MUTED, C_BG, v);
    #endif
    #if ENABLED(PIDTEMPBED)
      const auto &bp = thermalManager.temp_bed.pid;
      text(10, 34, F_M, C_TEXT, C_BG, "熱床");
      sprintf_P(v, PSTR("P %s  I %s  D %s"), f2(a, bp.p()), f2(b, bp.i()), f2(c, bp.d()));
      text(60, 36, F_S, C_MUTED, C_BG, v);
    #endif
    const bool busy = pid_state == 1 || pid_state == 2;
    if (busy) {
      sprintf_P(v, PSTR("調校中…　噴頭 %d°　熱床 %d°"), thermalManager.wholeDegHotend(0), thermalManager.wholeDegBed());
      text(10, 70, F_M, C_WARN, C_BG, v);
      text(10, 96, F_S, C_MUTED, C_BG, "加熱、冷卻來回 8 次，算好會自動套用。");
    }
    else if (pid_state == 3) {
      text(10, 70, F_M, C_OK, C_BG, "調校完成，新值已套用");
      text(10, 96, F_S, C_MUTED, C_BG, "要保留請按右上角「存檔」。");
    }
    else {
      text(10, 70, F_S, C_MUTED, C_BG, "噴頭用 220°、熱床用 60° 調校，跟平常列印的溫度接近。");
      text(10, 88, F_S, C_MUTED, C_BG, "調校時別碰噴頭和熱床；完成後按右上角「存檔」。");
    }

    if (busy) {
      Btn s; s.icon = &qqs_i28_player_stop; s.label = "停止調校"; s.fg = C_STOP; s.border = C_STOP;
      button(4, 176, 312, 60, s, pid_stop);
    }
    else {
      Btn h; h.icon = &qqs_i28_temperature; h.label = "噴頭 220°"; h.enabled = !job_active(); button(4, 176, 154, 60, h, pid_hot);
      Btn d; d.icon = &qqs_i28_bed; d.label = "熱床 60°"; d.enabled = !job_active(); button(162, 176, 154, 60, d, pid_bed);
    }
  }

  void go_pid() { if (pid_state == 3) pid_state = 0; ui.goto_screen(pid_screen); }

  // =========================================================================
  // Machine parameters
  // =========================================================================

  static void pr_esteps(const float d) {
    char cmd[32], a[16];
    sprintf_P(cmd, PSTR("M92 E%s"), f2(a, planner.settings.axis_steps_per_mm[E_AXIS] + d));
    run(cmd);
    redraw();
  }
  static void pr_em1()  { pr_esteps(-1); }
  static void pr_em01() { pr_esteps(-0.1f); }
  static void pr_ep01() { pr_esteps(+0.1f); }
  static void pr_ep1()  { pr_esteps(+1); }
  static void pr_n5()   { qqs_settings.test_count = 5;  redraw(); }
  static void pr_n10()  { qqs_settings.test_count = 10; redraw(); }
  static void pr_oneway() { qqs_settings.one_way_approach ^= 1; redraw(); }
  static void pr_soakm() { if (qqs_settings.soak_minutes) qqs_settings.soak_minutes--; redraw(); }
  static void pr_soakp() { if (qqs_settings.soak_minutes < 5) qqs_settings.soak_minutes++; redraw(); }
  static void pr_diff()  { qqs_settings.show_remeasure_diff ^= 1; redraw(); }

  static void params_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    save_feedback_poll();
    if (!live(next_ms, 1000)) return;

    touch.clear();
    header_save("機器參數", go_settings);
    char v[24], a[16];
    constexpr int16_t y0 = HEADER_H + 4, pitch = 41;
    auto label = [&](const uint8_t r, const char *t, const char *sub) {
      canvas(0, y0 + r * pitch, 120, 36, C_BG);
      text(8, sub ? 0 : 7, F_M, C_TEXT, C_BG, t);
      if (sub) text(8, 20, F_S, C_DIM, C_BG, sub);
    };

    label(0, "E 步數", f2(a, planner.settings.axis_steps_per_mm[E_AXIS]));
    { static const char * const l[4] = { "−1", "−0.1", "+0.1", "+1" };
      static const screenFunc_t cb[4] = { pr_em1, pr_em01, pr_ep01, pr_ep1 };
      for (uint8_t i = 0; i < 4; i++) { Btn b; b.label = l[i]; button(124 + i * 49, y0, 45, 34, b, cb[i]); } }

    label(1, "一致性測試", "每塔次數");
    { Btn b; b.label = "5 次";  if (qqs_settings.test_count != 10) { b.fg = C_ACCENT; b.border = C_ACCENT; } button(124, y0 + pitch, 94, 34, b, pr_n5); }
    { Btn b; b.label = "10 次"; if (qqs_settings.test_count == 10) { b.fg = C_ACCENT; b.border = C_ACCENT; } button(222, y0 + pitch, 94, 34, b, pr_n10); }

    label(2, "從上往下逼近", "往上先多抬 0.2");
    pill(252, y0 + 2 * pitch + 1, qqs_settings.one_way_approach, pr_oneway);

    label(3, "到溫穩定等待", "校準開始前");
    { Btn b; b.label = "−"; button(160, y0 + 3 * pitch, 44, 34, b, pr_soakm); }
    canvas(204, y0 + 3 * pitch, 64, 34, C_BG);
    sprintf_P(v, PSTR("%u 分"), qqs_settings.soak_minutes);
    text_c(0, 7, 64, F_M, C_TEXT, C_BG, v);
    { Btn b; b.label = "+"; button(272, y0 + 3 * pitch, 44, 34, b, pr_soakp); }

    label(4, "顯示兩次相差", "重新歸零再量後");
    pill(252, y0 + 4 * pitch + 1, qqs_settings.show_remeasure_diff, pr_diff);
  }

  void go_params() { ui.goto_screen(params_screen); }

  // =========================================================================
  // Feature switches
  // =========================================================================

  static void tg_runout() {
    #if HAS_FILAMENT_SENSOR
      run(runout.enabled ? "M412 S0" : "M412 S1");
    #endif
    redraw();
  }
  static void tg_probe() { qqs_settings.probe_installed ^= 1; redraw(); }
  static void tg_anim()  { qqs_settings.anim_off ^= 1; redraw(); }

  static void toggles_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    save_feedback_poll();
    if (!live(next_ms, 1000)) return;

    touch.clear();
    header_save("功能開關", go_settings);
    constexpr int16_t y0 = HEADER_H + 6, pitch = 50;
    auto row = [&](const uint8_t r, const char *t, const char *sub, const bool on, screenFunc_t cb) {
      canvas(0, y0 + r * pitch, 244, 44, C_BG);
      text(10, 2, F_M, C_TEXT, C_BG, t);
      text(10, 24, F_S, C_DIM, C_BG, sub);
      pill(252, y0 + r * pitch + 6, on, cb);
    };
    row(0, "斷料感測器", "這台原廠沒裝；裝了再打開", TERN0(HAS_FILAMENT_SENSOR, runout.enabled), tg_runout);
    row(1, "已安裝探頭", "感測器頁顯示探頭；自動校準要用", qqs_settings.probe_installed, tg_probe);
    row(2, "列印中動畫", "關掉就顯示靜態圖，完全不佔 CPU", !qqs_settings.anim_off, tg_anim);
    canvas(0, y0 + 3 * pitch, TFT_WIDTH, 30, C_BG);
    text(10, 6, F_S, C_MUTED, C_BG, "改完按右上角「存檔」，重開機才會保留。");
  }

  void go_toggles() { ui.goto_screen(toggles_screen); }

  // =========================================================================
  // G-code terminal
  // =========================================================================

  #define TERM_LINES 24
  #define TERM_COLS  48
  #define TERM_SHOW  6
  #define TERM_IN    40
  #define TERM_HIST  5

  static char term_log[TERM_LINES][TERM_COLS + 1];
  static uint8_t term_head = 0, term_col = 0, term_scroll = 0;
  static bool term_open = false, term_from_home = false;
  static volatile bool term_dirty = false;
  static bool term_in_dirty = false;
  static char term_in[TERM_IN + 1];
  static uint8_t term_len = 0;
  static char term_hist[TERM_HIST][TERM_IN + 1];
  static uint8_t term_hist_n = 0, term_hist_pos = 0;

  static void term_newline() {
    term_head = (term_head + 1) % TERM_LINES;
    term_log[term_head][0] = '\0';
    term_col = 0;
  }
  static void term_put(const uint8_t c) {
    if (c == '\r') return;
    if (c == '\n') {
      if (!strcmp_P(term_log[term_head], PSTR("ok"))) { term_log[term_head][0] = '\0'; term_col = 0; return; }   // drop bare "ok"
      term_newline();
      term_dirty = true;
      return;
    }
    if (term_col >= TERM_COLS) term_newline();
    term_log[term_head][term_col++] = (c < 0x20 || c > 0x7E) ? '?' : char(c);
    term_log[term_head][term_col] = '\0';
    term_dirty = true;
  }
  static void term_puts(const char *s) { while (*s) term_put(*s++); term_put('\n'); }

  static void term_draw_log() {
    term_dirty = false;
    canvas(0, HEADER_H, TFT_WIDTH, TERM_SHOW * 15 + 4, C_BAR);
    // The newest line is term_head (may be empty while a line is being written)
    const uint8_t newest = term_log[term_head][0] ? term_head : (term_head + TERM_LINES - 1) % TERM_LINES;
    for (uint8_t r = 0; r < TERM_SHOW; r++) {
      const uint8_t back = TERM_SHOW - 1 - r + term_scroll;
      if (back >= TERM_LINES - 1) continue;
      const char *l = term_log[(newest + TERM_LINES - back) % TERM_LINES];
      if (*l) text(4, 2 + r * 15, F_S, *l == '>' ? C_ACCENT : C_TEXT, C_BAR, l);
    }
    if (term_scroll) text_r(TFT_WIDTH - 4, 2, F_S, C_WARN, C_BAR, "▲ 往前");
  }
  static void term_draw_in() {
    term_in_dirty = false;
    canvas(0, HEADER_H + TERM_SHOW * 15 + 6, TFT_WIDTH, 24, C_TILE);
    frame(0, 0, TFT_WIDTH, 24, C_ACCENT);
    char t[TERM_IN + 4];
    sprintf_P(t, PSTR("> %s_"), term_in);
    text(6, 3, F_M, C_TEXT, C_TILE, t);
  }

  static void term_add(const char c) { if (term_len < TERM_IN) { term_in[term_len++] = c; term_in[term_len] = '\0'; } term_in_dirty = true; }
  template<char C> static void tk() {
    // A space after a letter is natural; letters after digits start a new word automatically
    if (C >= 'A' && C <= 'Z' && term_len && term_in[term_len - 1] != ' ') term_add(' ');
    term_add(C);
  }
  static void tk_bs()  { if (term_len) term_in[--term_len] = '\0'; term_in_dirty = true; }
  static void tk_sp()  { if (term_len && term_in[term_len - 1] != ' ') term_add(' '); }
  static void tk_hist() {
    if (!term_hist_n) return;
    term_hist_pos = (term_hist_pos + 1) % term_hist_n;
    strcpy(term_in, term_hist[term_hist_pos]); term_len = strlen(term_in);
    term_in_dirty = true;
  }
  static void term_send_str(const char *cmd) {
    if (!*cmd) return;
    if (!run(cmd)) return;
    char e[TERM_IN + 3]; sprintf_P(e, PSTR("> %s"), cmd);
    term_puts(e);
    // history: newest first, no duplicates of the newest
    if (!term_hist_n || strcmp(term_hist[0], cmd)) {
      for (int8_t i = _MIN(term_hist_n, TERM_HIST - 1); i > 0; i--) strcpy(term_hist[i], term_hist[i - 1]);
      strcpy(term_hist[0], cmd);
      if (term_hist_n < TERM_HIST) term_hist_n++;
    }
    term_hist_pos = term_hist_n - 1;   // next ↑ wraps to the newest
    term_scroll = 0;
  }
  static void tk_send() {
    while (term_len && term_in[term_len - 1] == ' ') term_in[--term_len] = '\0';
    char cmd[TERM_IN + 1]; strcpy(cmd, term_in);
    term_send_str(cmd);
    term_in[0] = '\0'; term_len = 0; term_in_dirty = true;
  }
  static void tk_m503() { term_send_str("M503"); }
  static void tk_m119() { term_send_str("M119"); }
  static void tk_m114() { term_send_str("M114"); }
  static void tk_m500() { term_send_str("M500"); }
  static void term_up()   { if (term_scroll < TERM_LINES - TERM_SHOW - 1) term_scroll += 3; term_dirty = true; }
  static void term_down() { term_scroll = term_scroll > 3 ? term_scroll - 3 : 0; term_dirty = true; }
  static void term_back() {
    term_open = false;
    if (term_from_home) ui.return_to_status(); else go_settings();
  }

  static void term_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 60000)) {           // between full redraws: only what changed
      if (term_dirty) term_draw_log();
      if (term_in_dirty) term_draw_in();
      return;
    }

    touch.clear();
    header("G-code", term_back);
    term_draw_log();
    touch.add_control(BUTTON, 0, HEADER_H, TFT_WIDTH, (TERM_SHOW * 15 + 4) / 2, (intptr_t)term_up);
    touch.add_control(BUTTON, 0, HEADER_H + (TERM_SHOW * 15 + 4) / 2, TFT_WIDTH, (TERM_SHOW * 15 + 4) / 2, (intptr_t)term_down);
    term_draw_in();

    constexpr int16_t ky = HEADER_H + TERM_SHOW * 15 + 32, kh = 27, kp = 29;
    static const char row1[] = "GMTSEFXYZP";
    static const screenFunc_t row1_cb[10] = { tk<'G'>, tk<'M'>, tk<'T'>, tk<'S'>, tk<'E'>, tk<'F'>, tk<'X'>, tk<'Y'>, tk<'Z'>, tk<'P'> };
    static const char row2[] = "1234567890.";
    static const screenFunc_t row2_cb[11] = { tk<'1'>, tk<'2'>, tk<'3'>, tk<'4'>, tk<'5'>, tk<'6'>, tk<'7'>, tk<'8'>, tk<'9'>, tk<'0'>, tk<'.'> };
    char k[2] = { 0, 0 };
    for (uint8_t i = 0; i < 10; i++) { k[0] = row1[i]; Btn b; b.label = k; button(1 + i * kp, ky, kp - 2, kh, b, row1_cb[i]); }
    { Btn b; b.label = "←"; b.fg = C_WARN; button(1 + 10 * kp, ky, kp - 2, kh, b, tk_bs); }
    for (uint8_t i = 0; i < 11; i++) { k[0] = row2[i]; Btn b; b.label = k; button(1 + i * kp, ky + kp, kp - 2, kh, b, row2_cb[i]); }

    const int16_t y3 = ky + 2 * kp;
    { Btn b; b.label = "-";    button(1,   y3, 27, kh, b, tk<'-'>); }
    { Btn b; b.label = "空白"; b.font = &F_S; button(30,  y3, 36, kh, b, tk_sp); }
    { Btn b; b.label = "M503"; b.font = &F_S; b.fg = C_COLD; button(68,  y3, 42, kh, b, tk_m503); }
    { Btn b; b.label = "M119"; b.font = &F_S; b.fg = C_COLD; button(112, y3, 42, kh, b, tk_m119); }
    { Btn b; b.label = "M114"; b.font = &F_S; b.fg = C_COLD; button(156, y3, 42, kh, b, tk_m114); }
    { Btn b; b.label = "M500"; b.font = &F_S; b.fg = C_COLD; button(200, y3, 42, kh, b, tk_m500); }
    { Btn b; b.label = "↑";    button(244, y3, 27, kh, b, tk_hist); }
    { Btn b; b.label = "送出"; b.font = &F_S; b.fg = C_ACCENT; b.border = C_ACCENT; button(273, y3, 46, kh, b, tk_send); }
  }

  void go_term() {
    term_from_home = ui.on_status_screen();
    term_open = true;
    term_scroll = 0;
    ui.goto_screen(term_screen);
  }

  void term_capture(const uint8_t c) { if (term_open) term_put(c); }

} // namespace qqs

void qqs_term_putc(const uint8_t c) { qqs::term_capture(c); }

#endif // QQS_UI
