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

  void info_tabs(const bool sensors) {
    canvas(0, HEADER_H, TFT_WIDTH, 32, C_BG);
    Btn a; a.label = "機器資訊";
    Btn b; b.label = "感測器";
    (sensors ? b : a).fg = C_ACCENT; (sensors ? b : a).border = C_ACCENT;
    button(4, HEADER_H + 2, 154, 28, a, go_info);
    button(162, HEADER_H + 2, 154, 28, b, go_sensors);
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
    info_tabs(true);

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

  // Leave the wizard. keep = values already applied stay (they were saved); otherwise restore the originals.
  static void cal_leave(const bool restore) {
    if (restore) {
      cal_res.H = cal_orig_H; cal_res.R = cal_orig_R;
      LOOP_ABC(i) cal_res.adj[i] = cal_orig_adj[i];
      if (!run(cal_active ? "M2006 A\nM2006 S0" : "M2006 S0")) return;
    }
    ui.return_to_status();
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
    header("手動校準", go_settings);
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
    if (thermalManager.degTargetHotend(0) < 140) thermalManager.setTargetHotend(140, 0);
    if (thermalManager.degTargetBed() < 70) thermalManager.setTargetBed(70);
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
    if (cal_confirm_ms && PENDING(millis(), cal_confirm_ms + 3000)) { cal_confirm_ms = 0; cal_leave(true); return; }
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

  static void cal_abandon() { cal_leave(true); }
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
    if (!run("M2006 A\nM2006 S0\nM500")) return;
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

} // namespace qqs

#endif // QQS_UI
