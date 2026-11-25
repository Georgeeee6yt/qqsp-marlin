/**
 * QQS-Pro v3.3 UI — probe tools: probe check, probe height (M851 Z), repeatability, delta auto
 * calibration (G33) and the bilinear mesh (G29). The work runs as M2007 (qqs_tools.cpp); these screens
 * start it, follow qqs::pj and show the results. Text only for now (room left for pictures later).
 *
 * The probe is FIX_MOUNTED with PAUSE_BEFORE_DEPLOY_STOW: G30 / G33 / G29 stop and ask to put it on /
 * take it off; the QQS wait screen (qqs_ui_sys.cpp) shows that and comes back here afterwards.
 */
#include "../../../inc/MarlinConfig.h"

#if ENABLED(QQS_UI)

#include "qqs_ui_priv.h"
#include "qqs_tools.h"
#include "qqs_settings.h"
#include "../tft.h"
#include "../touch.h"
#include "../../../module/probe.h"
#include "../../../module/planner.h"
#include "../../../module/delta.h"
#include "../../../feature/bedlevel/bedlevel.h"
#include "../../../gcode/queue.h"

namespace qqs {

  void go_toggles();
  void go_mesh();
  void go_probe();
  void job_screen();

  static bool prb_seen = false;          // the probe was seen triggering since the hub opened
  static millis_t job_ms = 0;            // when the UI started the current M2007

  // "+0.123" / "-0.050" (3 decimals)
  static char *f3s(char *o, const float v) {
    long u = lroundf(v * 1000.0f);
    const char sg = u < 0 ? '-' : '+';
    u = labs(u);
    sprintf_P(o, PSTR("%c%ld.%03ld"), sg, u / 1000, u % 1000);
    return o;
  }
  // "+0.05" for the mesh cells
  static char *f2s(char *o, const float v) {
    long u = lroundf(v * 100.0f);
    const char sg = u < 0 ? '-' : '+';
    u = labs(u);
    sprintf_P(o, PSTR("%c%ld.%02ld"), sg, u / 100, u % 100);
    return o;
  }

  static bool probe_now() { return PROBE_TRIGGERED(); }
  // Unlock on an untriggered -> triggered EDGE: a normally-closed probe that is not plugged in reads
  // "triggered" all the time and must not unlock anything. prb_last starts at the state found on entry.
  static bool prb_last = false, prb_fresh = true;
  static bool probe_poll() {
    const bool now = qqs_settings.probe_installed && probe_now();
    if (prb_fresh) { prb_fresh = false; prb_last = now; return true; }
    if (now == prb_last) return false;
    if (now) prb_seen = true;
    prb_last = now;
    return true;
  }
  static bool can_start() { return qqs_settings.probe_installed && prb_seen && !job_active() && !pj.running; }

  static void job_start(const char kind, const char *cmd) {
    if (!can_start()) return;
    pj.done = false; pj.kind = kind;
    if (!run(cmd)) return;
    job_ms = millis();
    ui.goto_screen(job_screen);
  }

  // =========================================================================
  // Running (shared): progress until M2007 is done, then its result page
  // =========================================================================

  static void ph_paper_screen();
  static void rp_result_screen();
  static void dc_result_screen();
  static void ph_fail_screen();
  static float ph_z = 1.0f;

  static const char *job_title() {
    switch (pj.kind) {
      case 'P': return "探頭高度";
      case 'R': return "探頭一致性";
      case 'C': return "Delta 自動校準";
      default:  return "自動網格";
    }
  }

  static void job_noback() { ui.set_status("量測中不能離開"); redraw(); }

  void job_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (pj.done && !pj.running) {             // finished: go to its result
      switch (pj.kind) {
        case 'P': if (pj.ok) { ph_z = 1.0f; ui.goto_screen(ph_paper_screen); } else ui.goto_screen(ph_fail_screen); return;
        case 'R': ui.goto_screen(rp_result_screen); return;
        case 'C': ui.goto_screen(dc_result_screen); return;
        default:  if (!pj.ok) ui.set_status("網格沒有建立完成"); go_mesh(); return;
      }
    }
    if (!live(next_ms, 1000)) return;
    touch.clear();
    header(job_title(), job_noback);
    canvas(0, HEADER_H, TFT_WIDTH, TFT_HEIGHT - HEADER_H, C_BG);

    char v[48];
    const char *m = ui.status_message;
    switch (pj.kind) {
      case 'R': sprintf_P(v, PSTR("量第 %u/%u 次"), pj.i + (pj.running ? 1 : 0), pj.n ? pj.n : 10); break;   // drawn with F_L
      case 'P': strcpy_P(v, pj.running ? PSTR("歸零後用探頭量中心…") : PSTR("準備中…")); break;   // drawn with F_L
      case 'C':
        if (!strncmp_P(m, PSTR("Iteration"), 9)) sprintf_P(v, PSTR("第 %d 輪"), atoi(m + 11));   // drawn with F_L
        else strcpy_P(v, PSTR("量測中…"));   // drawn with F_L
        break;
      default: {
        const char *p = strstr_P(m, PSTR("Point "));     // "Probing Point 12/81"
        if (p) sprintf_P(v, PSTR("量第 %s 點"), p + 6); else strcpy_P(v, PSTR("歸零中…"));   // drawn with F_L
      }
    }
    text_c(0, 40, TFT_WIDTH, F_L, C_TEXT, C_BG, v);
    const uint32_t s = (millis() - job_ms) / 1000;
    sprintf_P(v, PSTR("已經 %lu:%02lu"), (unsigned long)(s / 60), (unsigned long)(s % 60));
    text_c(0, 90, TFT_WIDTH, F_M, C_MUTED, C_BG, v);
    text_c(0, 130, TFT_WIDTH, F_S, C_MUTED, C_BG, "出現「請裝上探頭」「請取下探頭」時照做，");
    text_c(0, 148, TFT_WIDTH, F_S, C_MUTED, C_BG, "按「繼續」會回到這裡。量的時候不要碰機器。");
  }

  // =========================================================================
  // Hub: probe check + the three tools
  // =========================================================================

  static void go_heights() ;
  static void go_rep()  { job_start('R', "M2007 R10"); }
  static void go_dcal();

  static void probe_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (probe_poll()) redraw();
    const bool now = prb_last;
    if (!live(next_ms, 2000)) return;

    touch.clear();
    header("自動校準", go_settings);
    canvas(0, HEADER_H, TFT_WIDTH, 92, C_BG);
    char v[48], a[16], b[16];

    if (!qqs_settings.probe_installed) {
      text(10, 6,  F_M, C_WARN, C_BG, "沒有設定探頭");
      text(10, 34, F_S, C_TEXT, C_BG, "自動校準和網格要用探頭。");
      text(10, 52, F_S, C_TEXT, C_BG, "裝好後到「功能開關」打開「已安裝探頭」。");
      Btn t; t.icon = &qqs_i18_toggle_right; t.label = "功能開關";
      button(4, 182, 312, 54, t, go_toggles);
      return;
    }

    text(10, 4, F_M, C_TEXT, C_BG, "探頭");
    text(60, 4, F_M, now ? C_OK : C_MUTED, C_BG, now ? "已觸發" : "未觸發");
    text(10, 30, F_S, prb_seen ? C_OK : C_WARN, C_BG, prb_seen ? "探頭有反應，可以開始。" : "裝上探頭，用手按一下，確認有反應。");
    sprintf_P(v, PSTR("探頭高度（M851 Z）　%s"), f2(a, probe.offset.z));
    text(10, 50, F_S, C_MUTED, C_BG, v);
    if (pj.done && pj.ok && pj.kind == 'R') {
      float lo = pj.z[0], hi = pj.z[0];
      for (uint8_t i = 1; i < pj.i; i++) { NOMORE(lo, pj.z[i]); NOLESS(hi, pj.z[i]); }
      sprintf_P(v, PSTR("上次一致性：範圍 %s"), f3s(b, hi - lo));
      text(10, 68, F_S, C_DIM, C_BG, v);
    }
    else if (pj.done && pj.ok && pj.kind == 'C') {
      sprintf_P(v, PSTR("上次校準：標準差 %s"), pj.sd);
      text(10, 68, F_S, C_DIM, C_BG, v);
    }

    const bool en = can_start();
    Btn h; h.icon = &qqs_i18_arrows_vertical; h.label = "探頭高度"; h.enabled = en;
    button(4, 126, 101, 52, h, go_heights);
    Btn r; r.icon = &qqs_i18_refresh; r.label = "一致性"; r.enabled = en;
    button(110, 126, 101, 52, r, go_rep);
    Btn d; d.icon = &qqs_i18_target; d.label = "Delta 校準"; d.enabled = en;
    button(216, 126, 100, 52, d, go_dcal);
    Btn m; m.icon = &qqs_i18_grid_dots; m.label = "自動網格";
    button(4, 184, 312, 52, m, go_mesh);
  }

  void go_probe() {
    if (job_active()) { ui.set_status("列印中不能校準"); redraw(); return; }
    prb_seen = false; prb_fresh = true;
    ui.goto_screen(probe_screen);
  }

  // =========================================================================
  // Probe height (M851 Z): probe the centre, then the paper test at the same spot
  // =========================================================================

  static void ph_start() { job_start('P', "M2007 P"); }

  static void ph_intro_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 60000)) return;
    touch.clear();
    header("探頭高度", go_probe);
    canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
    char v[48], a[16];
    text(10, 4,  F_S, C_WARN, C_BG, "先做過手動校準：中心的 Z0 要準。");
    text(10, 26, F_S, C_TEXT, C_BG, "1. 按「開始」：歸零，用探頭量中心。");
    text(10, 44, F_S, C_TEXT, C_BG, "2. 提示「請取下探頭」時取下。");
    text(10, 62, F_S, C_TEXT, C_BG, "3. 噴嘴停在中心上方 1 mm，");
    text(10, 80, F_S, C_TEXT, C_BG, "    用紙片調到拉紙有一點阻力，按「好了」。");
    sprintf_P(v, PSTR("目前 M851 Z　%s"), f2(a, probe.offset.z));
    text(10, 108, F_S, C_MUTED, C_BG, v);
    Btn s; s.icon = &qqs_i18_player_play; s.label = "開始"; s.fg = C_ACCENT; s.border = C_ACCENT; s.enabled = can_start();
    button(4, 182, 312, 54, s, ph_start);
  }
  static void go_heights() { if (can_start()) ui.goto_screen(ph_intro_screen); }

  static void ph_cancel() { if (run("M2006 S0")) go_probe(); }

  static void ph_adj(const float d) {
    const float z = constrain(ph_z + d, -2.0f, 5.0f);
    char cmd[32], a[16];
    sprintf_P(cmd, PSTR("M2006 Z%s"), f2(a, z));
    if (run(cmd)) ph_z = z;
    redraw();
  }
  static void ph_m50() { ph_adj(-0.5f); }
  static void ph_m10() { ph_adj(-0.1f); }
  static void ph_m02() { ph_adj(-0.02f); }
  static void ph_p02() { ph_adj(+0.02f); }
  static void ph_p10() { ph_adj(+0.1f); }
  static void ph_p50() { ph_adj(+0.5f); }

  static float ph_new = 0;
  static void ph_result_screen();
  static void ph_done() {
    // The probe said the centre is at pj.measured; the nozzle touches it at ph_z
    ph_new = probe.offset.z - (pj.measured - ph_z);
    ui.goto_screen(ph_result_screen);
  }

  static void ph_paper_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 400)) return;
    touch.clear();
    header("探頭高度：紙片", ph_cancel);
    canvas(0, HEADER_H, TFT_WIDTH, 88, C_BG);
    char a[16];
    text_c(0, 0, TFT_WIDTH, F_XL, C_TEXT, C_BG, f2(a, ph_z));
    const bool busy = queue.has_commands_queued() || planner.has_blocks_queued();
    text_c(0, 48, TFT_WIDTH, F_S, busy ? C_WARN : C_MUTED, C_BG, busy ? "移動中…" : "噴嘴高度 Z：調到拉紙有一點阻力");
    static const char * const lab[6] = { "−0.5", "−0.1", "−0.02", "+0.02", "+0.1", "+0.5" };
    static const screenFunc_t cb[6] = { ph_m50, ph_m10, ph_m02, ph_p02, ph_p10, ph_p50 };
    for (uint8_t i = 0; i < 6; i++) { Btn b_; b_.label = lab[i]; button(4 + i * 53, 124, 49, 50, b_, cb[i]); }
    Btn c; c.icon = &qqs_i18_x; c.label = "放棄"; button(4, 180, 154, 56, c, ph_cancel);
    Btn d; d.icon = &qqs_i18_check; d.label = "好了"; d.fg = C_OK; d.border = C_OK; d.enabled = !busy;
    button(162, 180, 154, 56, d, ph_done);
  }

  static void ph_save() {
    char cmd[48], a[16];
    sprintf_P(cmd, PSTR("M851 Z%s\nM2006 S0\nM500"), f2(a, ph_new));
    if (run(cmd)) { ui.set_status("探頭高度已存"); go_probe(); }
  }

  static void ph_result_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 60000)) return;
    touch.clear();
    header("探頭高度：結果", ph_cancel);
    canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
    char v[48], a[16], b[16];
    sprintf_P(v, PSTR("探頭量到中心　%s"), f3s(a, pj.measured)); text(10, 6, F_S, C_TEXT, C_BG, v);
    sprintf_P(v, PSTR("紙片量到中心　%s"), f3s(a, ph_z));        text(10, 24, F_S, C_TEXT, C_BG, v);
    sprintf_P(v, PSTR("M851 Z  %s → %s"), f2(a, probe.offset.z), f2(b, ph_new));   // drawn with F_L
    text_c(0, 52, TFT_WIDTH, F_L, C_ACCENT, C_BG, v);
    const bool sane = WITHIN(ph_new, Z_PROBE_OFFSET_RANGE_MIN, Z_PROBE_OFFSET_RANGE_MAX);
    text(10, 100, F_S, sane ? C_MUTED : C_STOP, C_BG, sane ? "存了之後自動校準、網格都用這個高度。" : "超出合理範圍，請重量。");
    Btn c; c.icon = &qqs_i18_x; c.label = "放棄"; button(4, 180, 154, 56, c, ph_cancel);
    Btn s; s.icon = &qqs_i18_device_floppy; s.label = "存檔"; s.fg = C_ACCENT; s.border = C_ACCENT; s.enabled = sane;
    button(162, 180, 154, 56, s, ph_save);
  }

  static void ph_fail_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 60000)) return;
    touch.clear();
    header(job_title(), ph_cancel);
    canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
    text_c(0, 30, TFT_WIDTH, F_M, C_STOP, C_BG, "探頭沒有量到");
    text_c(0, 64, TFT_WIDTH, F_S, C_MUTED, C_BG, "檢查探頭有沒有裝好、接線、觸發方向（感測器頁）。");
    Btn c; c.icon = &qqs_i18_chevron_left; c.label = "返回"; button(4, 180, 312, 56, c, ph_cancel);
  }

  // =========================================================================
  // Repeatability result
  // =========================================================================

  static void rp_result_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 60000)) return;
    touch.clear();
    header("探頭一致性", go_probe);
    canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
    char v[48], a[16];
    if (!pj.ok || pj.i < 2) {
      text_c(0, 30, TFT_WIDTH, F_M, C_STOP, C_BG, "探頭沒有量到");
      text_c(0, 64, TFT_WIDTH, F_S, C_MUTED, C_BG, "檢查探頭有沒有裝好、接線、觸發方向（感測器頁）。");
    }
    else {
      float lo = pj.z[0], hi = pj.z[0], sum = 0, sq_ = 0;
      for (uint8_t i = 0; i < pj.i; i++) { NOMORE(lo, pj.z[i]); NOLESS(hi, pj.z[i]); sum += pj.z[i]; }
      const float mean = sum / pj.i;
      for (uint8_t i = 0; i < pj.i; i++) sq_ += sq(pj.z[i] - mean);
      const float range = hi - lo, sd = SQRT(sq_ / pj.i);
      const uint16_t col = range <= 0.02f ? C_OK : range <= 0.04f ? C_WARN : C_STOP;
      sprintf_P(v, PSTR("範圍 %s"), f3s(a, range) + 1);   // drawn with F_L
      text_c(0, 2, TFT_WIDTH, F_L, col, C_BG, v);
      sprintf_P(v, PSTR("標準差 %s　共 %u 次"), f3s(a, sd) + 1, pj.i);
      text_c(0, 40, TFT_WIDTH, F_S, C_MUTED, C_BG, v);
      // Each sample relative to the first, two rows of five
      for (uint8_t i = 0; i < pj.i; i++)
        text_c(4 + (i % 5) * 62, 64 + (i / 5) * 18, 62, F_S, C_TEXT, C_BG, f3s(a, pj.z[i] - pj.z[0]));
      text_c(0, 110, TFT_WIDTH, F_S, col, C_BG,
        range <= 0.02f ? "很穩定，可以做自動校準。" : range <= 0.04f ? "偏大：檢查探頭固定、噴嘴上有沒有殘料。" : "太大：先別做自動校準，檢查探頭。");
    }
    Btn r; r.icon = &qqs_i18_refresh; r.label = "再測一次"; r.enabled = can_start(); button(4, 180, 154, 56, r, go_rep);
    Btn b; b.icon = &qqs_i18_chevron_left; b.label = "返回"; button(162, 180, 154, 56, b, go_probe);
  }

  // =========================================================================
  // Delta auto calibration (G33)
  // =========================================================================

  static uint8_t dc_points = 3;
  static void dc_p3() { dc_points = 3; redraw(); }
  static void dc_p6() { dc_points = 6; redraw(); }
  static void dc_start() {
    char cmd[16];
    sprintf_P(cmd, PSTR("M2007 C P%u"), dc_points);
    job_start('C', cmd);
  }

  static void dc_intro_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 60000)) return;
    touch.clear();
    header("Delta 自動校準", go_probe);
    canvas(0, HEADER_H, TFT_WIDTH, 92, C_BG);
    char v[48], a[16], b[16];
    text(10, 4,  F_S, C_TEXT, C_BG, "用探頭量多點，自動算 H、三塔、半徑、塔角。");
    text(10, 22, F_S, C_TEXT, C_BG, "第一層偏移會保留。先做好「探頭高度」。");
    text(10, 40, F_S, C_MUTED, C_BG, "大約 5～15 分鐘，量的時候不要碰機器。");
    sprintf_P(v, PSTR("目前 H %s　R %s"), f2(a, delta_height), f2(b, delta_radius));
    text(10, 66, F_S, C_DIM, C_BG, v);
    Btn p3; p3.label = "7 點"; p3.sub = "中心、三塔、對面"; if (dc_points == 3) { p3.fg = C_ACCENT; p3.border = C_ACCENT; }
    button(4, 128, 154, 48, p3, dc_p3);
    Btn p6; p6.label = "多點平均"; p6.sub = "比較久"; if (dc_points == 6) { p6.fg = C_ACCENT; p6.border = C_ACCENT; }
    button(162, 128, 154, 48, p6, dc_p6);
    Btn s; s.icon = &qqs_i18_player_play; s.label = "開始"; s.fg = C_ACCENT; s.border = C_ACCENT; s.enabled = can_start();
    button(4, 182, 312, 54, s, dc_start);
  }
  static void go_dcal() { if (can_start()) ui.goto_screen(dc_intro_screen); }

  static void dc_undo() { if (run("M2007 U")) { ui.set_status("已還原"); go_probe(); } }
  static void dc_save() { if (run("M500")) { ui.set_status("校準結果已存"); go_probe(); } }

  static void dc_result_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 60000)) return;
    touch.clear();
    header("Delta 自動校準：結果", dc_undo);
    canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
    char v[56], a[16], b[16];
    if (!pj.ok) {
      text_c(0, 24, TFT_WIDTH, F_M, C_STOP, C_BG, "沒有算出結果");
      text_c(0, 58, TFT_WIDTH, F_S, C_MUTED, C_BG, "探頭沒量到，或點太偏。按「還原」讀回存檔的值。");
      Btn u; u.icon = &qqs_i18_restore; u.label = "還原"; button(4, 180, 312, 56, u, dc_undo);
      return;
    }
    sprintf_P(v, PSTR("標準差 %s mm"), pj.sd);   // drawn with F_L
    const float sdv = atof(pj.sd);
    text_c(0, 0, TFT_WIDTH, F_L, sdv <= 0.02f ? C_OK : sdv <= 0.04f ? C_WARN : C_STOP, C_BG, v);
    struct Row { const char *n; float o, n2; } rows[] = {
      { "H",    pj.oldH,       pj.newH },
      { "X 塔", pj.old_adj[0], pj.new_adj[0] },
      { "Y 塔", pj.old_adj[1], pj.new_adj[1] },
      { "Z 塔", pj.old_adj[2], pj.new_adj[2] },
      { "半徑", pj.oldR,       pj.newR },
    };
    for (uint8_t i = 0; i < COUNT(rows); i++) {
      text(20, 40 + i * 17, F_S, C_MUTED, C_BG, rows[i].n);
      sprintf_P(v, PSTR("%s → %s"), f2(a, rows[i].o), f2(b, rows[i].n2));
      text_r(300, 40 + i * 17, F_S, C_TEXT, C_BG, v);
    }
    char c3[16];
    sprintf_P(v, PSTR("塔角 %s %s %s"), f2(a, pj.new_ang[0]), f2(b, pj.new_ang[1]), f2(c3, pj.new_ang[2]));
    text(20, 128, F_S, C_DIM, C_BG, v);
    if (pj.had_mesh) text_r(316, 128, F_S, C_WARN, C_BG, "網格已清除，要重建");
    Btn u; u.icon = &qqs_i18_restore; u.label = "還原"; button(4, 180, 154, 56, u, dc_undo);
    Btn s; s.icon = &qqs_i18_device_floppy; s.label = "存檔"; s.fg = C_ACCENT; s.border = C_ACCENT; button(162, 180, 154, 56, s, dc_save);
  }

  // =========================================================================
  // Mesh (G29): build / on-off / save, grid view
  // =========================================================================

  static void me_build()  { job_start('M', "M2007 M"); }
  static void me_toggle() { if (run(planner.leveling_active ? "M420 S0" : "M420 S1")) ui.set_status(planner.leveling_active ? "網格關閉" : "網格使用中"); redraw(); }
  static void me_save()   { if (run("M500")) ui.set_status("網格已存"); redraw(); }

  static void mesh_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (probe_poll() && prb_seen && !pj.running) redraw();
    if (!live(next_ms, 2000)) return;
    touch.clear();
    header("自動網格", go_settings);
    canvas(0, HEADER_H, TFT_WIDTH, 152, C_BG);
    char v[48], a[16];

    if (!bedlevel.has_mesh()) {
      text_c(0, 50, TFT_WIDTH, F_M, C_MUTED, C_BG, "還沒有網格");
      text_c(0, 84, TFT_WIDTH, F_S, C_DIM, C_BG, "按「建立網格」用探頭量整個熱床（約 10 分鐘）。");
    }
    else {
      // 9 x 9 cells, +Y up. Points outside the probing radius were extrapolated: dim.
      constexpr int16_t CW = 34, CH = 14, X0 = 7, Y0 = 2;
      constexpr float R = DELTA_PRINTABLE_RADIUS - (PROBING_MARGIN);
      float lo = 99, hi = -99;
      for (uint8_t j = 0; j < GRID_MAX_POINTS_Y; j++)
        for (uint8_t i = 0; i < GRID_MAX_POINTS_X; i++) {
          const float z = bedlevel.z_values[i][j];
          const float x = bedlevel.get_mesh_x(i), y = bedlevel.get_mesh_y(j);
          const bool in = HYPOT(x, y) <= R + 0.5f;
          if (isnan(z)) continue;
          if (in) { NOMORE(lo, z); NOLESS(hi, z); }
          const uint16_t c = !in ? C_DIM : z > 0.05f ? C_HOT : z < -0.05f ? C_COLD : C_TEXT;
          text_c(X0 + i * CW, Y0 + (GRID_MAX_POINTS_Y - 1 - j) * CH, CW, F_S, c, C_BG, f2s(a, z));
        }
      char r[16];
      sprintf_P(v, PSTR("範圍 %s　%s"), f2s(r, hi - lo) + 1, planner.leveling_active ? "使用中" : "關閉");
      text_c(0, 132, TFT_WIDTH, F_S, planner.leveling_active ? C_OK : C_WARN, C_BG, v);
    }

    const bool en = can_start();
    Btn b; b.icon = &qqs_i18_grid_dots; b.label = "建立網格"; b.enabled = en;
    if (!en && !pj.running) { b.icon = nullptr; b.sub = qqs_settings.probe_installed ? "先按一下探頭" : "沒有設定探頭"; }
    button(4, 186, 101, 50, b, me_build);
    Btn t; t.icon = &qqs_i18_toggle_right; t.label = planner.leveling_active ? "關閉網格" : "使用網格"; t.enabled = bedlevel.has_mesh() && !job_active();
    button(110, 186, 101, 50, t, me_toggle);
    Btn s; s.icon = &qqs_i18_device_floppy; s.label = "存檔"; s.enabled = bedlevel.has_mesh() && !job_active();
    button(216, 186, 100, 50, s, me_save);
  }

  void go_mesh() {
    if (job_active()) { ui.set_status("列印中不能建立網格"); redraw(); return; }
    if (ui.currentScreen != job_screen) {        // coming back from building a mesh (or from the hub) keeps the check
      prb_fresh = true;
      if (ui.currentScreen != probe_screen) prb_seen = false;
    }
    ui.goto_screen(mesh_screen);
  }

} // namespace qqs

#endif // QQS_UI
