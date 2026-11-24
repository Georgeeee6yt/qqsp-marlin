/**
 * QQS-Pro v3 UI — screens (v3.0: boot, idle home, printing, value editors, stop confirm).
 *
 * Every Chinese string used here is collected by qqs_tools/gen_assets.py, which bakes
 * just those glyphs into qqs_assets.cpp. Re-run it after adding or changing text.
 * Lines that draw with F_L / F_XL must keep that token on the same line as the literal.
 */
#include "../../../inc/MarlinConfig.h"

#if ENABLED(QQS_UI)

#include "qqs_ui.h"
#include "qqs_draw.h"
#include "qqs_settings.h"
#include "../tft.h"
#include "../touch.h"
#include "../../marlinui.h"
#include "../../menu/menu.h"
#include "../../../MarlinCore.h"
#include "../../../module/temperature.h"
#include "../../../module/planner.h"
#include "../../../module/motion.h"
#include "../../../module/printcounter.h"
#include "../../../gcode/queue.h"
#include "../../../sd/cardreader.h"
#include "../../../libs/duration_t.h"

#include "../../../module/settings.h"
#include "../../../module/delta.h"
#include "qqs_motion.h"

#if ENABLED(BABYSTEPPING)
  #include "../../../feature/babystep.h"
#endif
#if ENABLED(ADVANCED_PAUSE_FEATURE)
  #include "../../../feature/pause.h"
#endif
#ifdef MKS_WIFI
  extern uint8_t mks_wifi_ip[4];
  extern uint8_t mks_wifi_net_state;
#endif


// ---------------------------------------------------------------------------
// Settings stored in EEPROM
// ---------------------------------------------------------------------------
qqs_settings_t qqs_settings;

void qqs_settings_reset() {
  memset(&qqs_settings, 0, sizeof(qqs_settings));
  qqs_settings.layer_offset        = 0.10f;   // v2.2: H 355.72 calibrated, printed at 355.62
  qqs_settings.test_count          = 5;
  qqs_settings.soak_minutes        = 0;
  qqs_settings.one_way_approach    = 1;
  qqs_settings.show_remeasure_diff = 1;
}

namespace qqs {

  #define PREHEAT_NOZZLE 140
  #define PREHEAT_BED     70

  static float bs_total = 0;          // Z babystep sum for the running job
  static bool  job_was_running = false;
  static bool  stopping = false;      // stop confirmed, waiting for the job to end

  static inline void redraw() { ui.refresh(LCDVIEW_REDRAW_NOW); }
  static inline bool job_active() { return printJobOngoing() || card.isFileOpen(); }
  static void go_home_screen() { ui.return_to_status(); }

  // Inject G-code from the UI. Only one injected batch at a time (it shares Marlin's 64-byte buffer).
  static bool run(const char *g) {
    if (queue.injected_commands_P || queue.injected_commands[0]) {
      ui.set_status("忙碌中，請稍候");
      redraw();
      return false;
    }
    queue.inject(g);
    return true;
  }

  // "12.34" / "-0.50" without printf float support
  static char *f2(char *o, const float v) {   // o: at least 16 bytes
    long h = lroundf(v * 100.0f);
    const bool neg = h < 0;
    if (neg) h = -h;
    sprintf_P(o, neg ? PSTR("-%ld.%02ld") : PSTR("%ld.%02ld"), h / 100, h % 100);
    return o;
  }

  // =========================================================================
  // Boot screen
  // =========================================================================

  void boot_screen() {
    tft.queue.reset();
    tft.fill(0, 0, TFT_WIDTH, TFT_HEIGHT, C_BG);

    const int16_t gx = TFT_WIDTH - qqs_boot_girl.w - 8, gy = 8;
    const int16_t fx = 10, fy = gy + (qqs_boot_girl.h - qqs_boot_flsun.h) / 2;

    canvas(0, 0, TFT_WIDTH, gy + qqs_boot_girl.h + 2, C_BG);
    bitmap(fx, fy, qqs_boot_flsun, QRGB(0xEB, 0xEB, 0xEB), C_BG);
    bitmap(gx, gy, qqs_boot_girl, C_ACCENT, C_BG);
    const int16_t cx = (fx + qqs_boot_flsun.w + gx) / 2, cy = fy + qqs_boot_flsun.h / 2;
    for (int8_t i = -4; i <= 4; i++) {          // small "x"
      fill(cx + i, cy + i, 2, 2, C_MUTED);
      fill(cx + i, cy - i, 2, 2, C_MUTED);
    }

    canvas(0, gy + qqs_boot_girl.h + 2, TFT_WIDTH, 24, C_BG);
    text_c(0, 2, TFT_WIDTH, F_M, C_TEXT, C_BG, "QQS-Pro  ·  Marlin " QQS_FW_VERSION);

    boot_progress(0, "啟動中…");
    tft.queue.sync();
  }

  void boot_progress(uint8_t percent, const char *msg) {
    const int16_t y = TFT_HEIGHT - 52;
    canvas(60, y, 200, 6, C_BG);
    fill(0, 0, 200, 6, QRGB(0x33, 0x33, 0x33));
    if (percent) fill(0, 0, 2 * _MIN(percent, 100), 6, C_ACCENT);
    canvas(0, y + 14, TFT_WIDTH, F_S.height + 2, C_BG);
    text_c(0, 0, TFT_WIDTH, F_S, C_MUTED, C_BG, msg);
  }

  void boot_finish(uint32_t ms_left) {
    for (uint8_t p = 10; p <= 100; p += 10) {
      boot_progress(p, p < 100 ? "啟動中…" : "完成");
      tft.queue.sync();
      safe_delay(ms_left / 10);
    }
  }

  // =========================================================================
  // Generic value editor (temperature, fan, speed, flow)
  // =========================================================================

  struct Editor {
    const char *title;
    const char *unit;            // "°" or "%"
    int16_t vmin, vmax;
    int16_t quick[5];
    bool show_current;           // temperatures: show the measured value
    int16_t (*get)();
    void (*set)(int16_t);
    int16_t (*current)();
  };

  static const Editor *ed = nullptr;

  static void ed_apply(int16_t v) {
    LIMIT(v, ed->vmin, ed->vmax);
    ed->set(v);
    redraw();
  }
  static void ed_m10() { ed_apply(ed->get() - 10); }
  static void ed_m1()  { ed_apply(ed->get() - 1); }
  static void ed_p1()  { ed_apply(ed->get() + 1); }
  static void ed_p10() { ed_apply(ed->get() + 10); }
  static void ed_q0()  { ed_apply(ed->quick[0]); }
  static void ed_q1()  { ed_apply(ed->quick[1]); }
  static void ed_q2()  { ed_apply(ed->quick[2]); }
  static void ed_q3()  { ed_apply(ed->quick[3]); }
  static void ed_q4()  { ed_apply(ed->quick[4]); }
  static void ed_back() { ui.return_to_status(); }

  static void editor_screen() {
    static millis_t next_ms = 0;
    if (ed->show_current && ELAPSED(millis(), next_ms)) { next_ms = millis() + 1000; redraw(); }
    if (!ui.should_draw()) return;

    touch.clear();
    header(ed->title, ed_back);

    // Value
    const int16_t v = ed->get();
    canvas(0, HEADER_H, TFT_WIDTH, 74, C_BG);
    char buf[16];
    if (v == 0 && ed->show_current) strcpy(buf, "關");
    else sprintf_P(buf, PSTR("%d%s"), v, ed->unit);
    const bool big_off = (v == 0 && ed->show_current);
    if (big_off) text_c(0, 10, TFT_WIDTH, F_L, C_MUTED, C_BG, buf);
    else         text_c(0, 4, TFT_WIDTH, F_XL, C_TEXT, C_BG, buf);
    if (ed->show_current) {
      char cur[24];
      sprintf_P(cur, PSTR("目前 %d°"), ed->current());
      text_c(0, 54, TFT_WIDTH, F_S, C_MUTED, C_BG, cur);
    }

    // Adjust row
    static const char * const adj[4] = { "−10", "−1", "+1", "+10" };
    static const screenFunc_t adj_cb[4] = { ed_m10, ed_m1, ed_p1, ed_p10 };
    for (uint8_t i = 0; i < 4; i++) {
      Btn b; b.label = adj[i]; b.font = &F_L;
      button(4 + i * 79, 110, 75, 52, b, adj_cb[i]);
    }

    // Quick picks
    static const screenFunc_t q_cb[5] = { ed_q0, ed_q1, ed_q2, ed_q3, ed_q4 };
    for (uint8_t i = 0; i < 5; i++) {
      const int16_t q = ed->quick[i];
      char qb[12];
      if (q == 0 && ed->show_current) strcpy(qb, "關");
      else sprintf_P(qb, PSTR("%d%s"), q, ed->unit);
      Btn b; b.label = qb;
      if (q == v) { b.border = C_ACCENT; b.fg = C_ACCENT; }
      button(4 + i * 63, 168, 60, 66, b, q_cb[i]);
    }
  }

  static int16_t get_noz() { return thermalManager.degTargetHotend(0); }
  static void    set_noz(int16_t v) { thermalManager.setTargetHotend(v, 0); if (v) thermalManager.start_watching_hotend(0); }
  static int16_t cur_noz() { return thermalManager.wholeDegHotend(0); }
  static int16_t get_bed() { return thermalManager.degTargetBed(); }
  static void    set_bed(int16_t v) { thermalManager.setTargetBed(v); if (v) thermalManager.start_watching_bed(); }
  static int16_t cur_bed() { return thermalManager.wholeDegBed(); }
  static int16_t get_fan() { return (thermalManager.fan_speed[0] * 100 + 127) / 255; }
  static void    set_fan(int16_t v) { thermalManager.set_fan_speed(0, (v * 255 + 50) / 100); }
  static int16_t get_spd() { return feedrate_percentage; }
  static void    set_spd(int16_t v) { feedrate_percentage = v; }
  static int16_t get_flw() { return planner.flow_percentage[0]; }
  static void    set_flw(int16_t v) { planner.set_flow(0, v); }

  static const Editor ed_nozzle = { "噴頭溫度", "°", 0, HEATER_0_MAXTEMP - (HOTEND_OVERSHOOT), { 0, 140, 200, 220, 240 }, true,  get_noz, set_noz, cur_noz };
  static const Editor ed_bedt   = { "熱床溫度", "°", 0, BED_MAX_TARGET,                          { 0, 50, 60, 70, 80 },     true,  get_bed, set_bed, cur_bed };
  static const Editor ed_fan    = { "風扇",     "%", 0, 100,                                     { 0, 30, 50, 75, 100 },    false, get_fan, set_fan, nullptr };
  static const Editor ed_speed  = { "列印速度", "%", 10, 300,                                    { 50, 75, 100, 125, 150 }, false, get_spd, set_spd, nullptr };
  static const Editor ed_flow   = { "擠出流量", "%", 50, 200,                                    { 90, 95, 100, 105, 110 }, false, get_flw, set_flw, nullptr };

  static void open_editor(const Editor &e) { ed = &e; ui.goto_screen(editor_screen); }
  static void go_nozzle() { open_editor(ed_nozzle); }
  static void go_bed()    { open_editor(ed_bedt); }
  static void go_fan()    { open_editor(ed_fan); }
  static void go_speed()  { open_editor(ed_speed); }
  static void go_flow()   { open_editor(ed_flow); }

  // =========================================================================
  // Stop confirmation
  // =========================================================================

  static void stop_cancel() { ui.return_to_status(); }
  static void stop_confirm() {
    stopping = true;
    ui.abort_print();             // EVENT_GCODE_SD_ABORT: lift, centre, G28
    ui.set_status("停止中…抬起、置中後歸零");
    ui.return_to_status();
  }

  static void stop_screen() {
    if (!ui.should_draw()) return;
    touch.clear();
    header("停止列印", stop_cancel);
    canvas(0, HEADER_H, TFT_WIDTH, 110, C_BG);
    text_c(0, 14, TFT_WIDTH, F_L, C_TEXT, C_BG, "確定要停止列印？");
    text_c(0, 52, TFT_WIDTH, F_S, C_MUTED, C_BG, "噴頭會先抬起、移到中間，再歸零。");
    text_c(0, 72, TFT_WIDTH, F_S, C_MUTED, C_BG, "這次列印無法再繼續。");

    Btn c; c.icon = &qqs_i28_x; c.label = "取消";
    button(4, 150, 154, 84, c, stop_cancel);
    Btn s; s.icon = &qqs_i28_player_stop; s.label = "確定停止"; s.fg = C_STOP; s.border = C_STOP; s.bg = C_STOPBG;
    button(162, 150, 154, 84, s, stop_confirm);
  }

  static void go_stop() { if (!stopping) ui.goto_screen(stop_screen); }

  // =========================================================================
  // Actions on the home screen
  // =========================================================================

  static void act_print() {
    // QQS-Pro has no card-detect pin: (re)mount on every tap, like the stock firmware.
    if (!job_active()) card.mount();
    if (card.isMounted()) ui.goto_screen(menu_media);
    else { ui.set_status("讀不到 SD 卡"); redraw(); }
  }
  static void act_preheat() {
    set_noz(PREHEAT_NOZZLE); set_bed(PREHEAT_BED);
    ui.set_status("預熱中 140°／70°");
    redraw();
  }
  static void act_cool() {
    thermalManager.disable_all_heaters();
    thermalManager.set_fan_speed(0, 0);
    ui.set_status("已關閉加熱");
    redraw();
  }
  static void act_home() {
    if (job_active()) return;
    if (run("G28\nM117")) ui.set_status("歸零中…");   // M117 with no text puts the normal status back
    redraw();
  }
  static void move_screen();
  static void filament_screen();
  static void settings_screen();
  static void act_move()     { ui.goto_screen(move_screen); }
  static void act_filament() { ui.goto_screen(filament_screen); }
  static void act_settings() { ui.goto_screen(settings_screen); }


  // =========================================================================
  // Move screen
  // =========================================================================

  static float move_step = 10;

  static void mv_to(xyz_pos_t t, const bool xy) {
    if (axes_should_home()) { ui.set_status("請先歸零"); redraw(); return; }
    if (xy) {
      // Too far? Back off along the move until it fits.
      const xyz_pos_t a = current_position;
      if (!reachable(t)) {
        float lo = 0, hi = 1;
        for (uint8_t n = 0; n < 16; n++) {
          const float m = (lo + hi) * 0.5f;
          const xyz_pos_t p = { a.x + (t.x - a.x) * m, a.y + (t.y - a.y) * m, t.z };
          if (reachable(p)) lo = m; else hi = m;
        }
        t.set(a.x + (t.x - a.x) * lo, a.y + (t.y - a.y) * lo, t.z);
        if (HYPOT(t.x - a.x, t.y - a.y) < 0.1f) { ui.set_status("已到邊界"); redraw(); return; }
      }
    }
    else {
      const float zmax = z_max_at(t.x, t.y);
      if (t.z > zmax) t.z = zmax;
      if (t.z < 0) t.z = 0;
      if (ABS(t.z - current_position.z) < 0.01f) { ui.set_status(t.z > 0 ? "已到頂" : "已到 Z0"); redraw(); return; }
    }
    char cmd[64], bx[16], by[16], bz[16];
    snprintf_P(cmd, sizeof(cmd), PSTR("G90\nG0 X%s Y%s Z%s F%d"), f2(bx, t.x), f2(by, t.y), f2(bz, t.z), xy ? 3000 : 1200);
    run(cmd);
    redraw();
  }
  static void mv_d(const float dx, const float dy, const float dz) {
    xyz_pos_t t = current_position; t.x += dx; t.y += dy; t.z += dz;
    mv_to(t, dz == 0);
  }
  static void mv_yp() { mv_d(0, move_step, 0); }
  static void mv_ym() { mv_d(0, -move_step, 0); }
  static void mv_xm() { mv_d(-move_step, 0, 0); }
  static void mv_xp() { mv_d(move_step, 0, 0); }
  static void mv_zp() { mv_d(0, 0, move_step); }
  static void mv_zm() { mv_d(0, 0, -move_step); }
  static void mv_center() { xyz_pos_t t = current_position; t.x = t.y = 0; mv_to(t, true); }
  static void mv_home() { if (run("G28\nM117")) ui.set_status("歸零中…"); redraw(); }
  static void mv_off()  { if (run("M84")) ui.set_status("馬達已放鬆，移動前要先歸零"); redraw(); }
  static void st_01() { move_step = 0.1f; redraw(); }
  static void st_1()  { move_step = 1;    redraw(); }
  static void st_10() { move_step = 10;   redraw(); }
  static void st_50() { move_step = 50;   redraw(); }

  static void move_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (ELAPSED(millis(), next_ms)) { next_ms = millis() + 500; redraw(); }
    if (!ui.should_draw()) return;

    touch.clear();
    header("移動", go_home_screen);

    // Position
    canvas(0, HEADER_H, TFT_WIDTH, 20, C_BG);
    if (axes_should_home())
      text_c(0, 2, TFT_WIDTH, F_S, C_WARN, C_BG, "尚未歸零，先按「歸零」");
    else {
      char buf[64], bx[16], by[16], bz[16];
      sprintf_P(buf, PSTR("X %s    Y %s    Z %s"), f2(bx, current_position.x), f2(by, current_position.y), f2(bz, current_position.z));
      text_c(0, 2, TFT_WIDTH, F_S, C_TEXT, C_BG, buf);
    }

    const bool homed = !axes_should_home();
    // XY pad
    constexpr int16_t ox = 4, oy = 54, cs = 50, g = 2;
    { Btn b; b.icon = &qqs_i28_arrow_up;    b.enabled = homed; button(ox + (cs + g), oy,               cs, cs, b, mv_yp); }
    { Btn b; b.icon = &qqs_i28_arrow_left;  b.enabled = homed; button(ox,            oy + (cs + g),    cs, cs, b, mv_xm); }
    { Btn b; b.label = "中心"; b.font = &F_S; b.icon = &qqs_i18_focus_centered; b.enabled = homed; button(ox + (cs + g), oy + (cs + g), cs, cs, b, mv_center); }
    { Btn b; b.icon = &qqs_i28_arrow_right; b.enabled = homed; button(ox + 2 * (cs + g), oy + (cs + g), cs, cs, b, mv_xp); }
    { Btn b; b.icon = &qqs_i28_arrow_down;  b.enabled = homed; button(ox + (cs + g), oy + 2 * (cs + g), cs, cs, b, mv_ym); }

    // Z
    { Btn b; b.icon = &qqs_i28_arrow_bar_up;   b.label = "Z+"; b.enabled = homed; button(162, 54, 56, 75, b, mv_zp); }
    { Btn b; b.icon = &qqs_i28_arrow_bar_down; b.label = "Z−"; b.enabled = homed; button(162, 133, 56, 75, b, mv_zm); }

    // Home / motors off
    { Btn b; b.icon = &qqs_i28_home;      b.label = "歸零";     button(222, 54, 94, 75, b, mv_home); }
    { Btn b; b.icon = &qqs_i28_lock_open; b.label = "放鬆馬達"; button(222, 133, 94, 75, b, mv_off); }

    // Step
    static const char * const st_lbl[4] = { "0.1 mm", "1 mm", "10 mm", "50 mm" };
    static const float st_val[4] = { 0.1f, 1, 10, 50 };
    static const screenFunc_t st_cb[4] = { st_01, st_1, st_10, st_50 };
    for (uint8_t i = 0; i < 4; i++) {
      Btn b; b.label = st_lbl[i];
      if (ABS(move_step - st_val[i]) < 0.01f) { b.fg = C_ACCENT; b.border = C_ACCENT; }
      button(4 + i * 79, 212, 75, 26, b, st_cb[i]);
    }
  }

  // =========================================================================
  // Filament screen + Bowden length calibration
  // =========================================================================

  #if ENABLED(ADVANCED_PAUSE_FEATURE)
    static float bowden() { return fc_settings[0].load_length; }
  #else
    static float bowden() { return FILAMENT_CHANGE_FAST_LOAD_LENGTH; }
  #endif

  static bool noz_ready() {
    const int16_t t = thermalManager.degTargetHotend(0);
    return t >= 180 && thermalManager.wholeDegHotend(0) >= t - 5;
  }

  static void fil_pla()  { set_noz(200); redraw(); }
  static void fil_petg() { set_noz(240); redraw(); }
  static void fil_load() {
    if (!noz_ready()) return;
    char cmd[64];
    const int fast = _MAX(0, int(bowden()) - 40);
    snprintf_P(cmd, sizeof(cmd), PSTR("M83\nG1 E%d F3000\nG1 E70 F300\nM82\nM117"), fast);
    if (run(cmd)) ui.set_status("進料中…快送後慢送，看到出料就好");
    redraw();
  }
  static void fil_unload() {
    if (!noz_ready()) return;
    char cmd[64];
    snprintf_P(cmd, sizeof(cmd), PSTR("M83\nG1 E5 F300\nG1 E-15 F3000\nG4 P2000\nG1 E-%d F3000\nM82\nM117"), _MIN(int(bowden()) + 40, EXTRUDE_MAXLENGTH - 10));
    if (run(cmd)) ui.set_status("退料中…");
    redraw();
  }
  static void fil_extrude() { if (noz_ready() && run("M83\nG1 E10 F300\nM82\nM117")) ui.set_status("擠出 10 mm"); redraw(); }
  static void fil_retract() { if (noz_ready() && run("M83\nG1 E-10 F1200\nM82\nM117")) ui.set_status("回抽 10 mm"); redraw(); }

  // --- Bowden calibration ---
  static uint8_t bc_state = 0;   // 0 intro, 1 feeding, 2 result, 3 stopping (waiting for M410)
  static bool bc_done = false;
  static float bc_e0 = 0, bc_len = 0;

  static float bc_fed() { return planner.get_axis_position_mm(E_AXIS) - bc_e0; }

  static void bc_start() {
    if (!noz_ready() || queue.has_commands_queued() || planner.has_blocks_queued()) return;
    bc_e0 = planner.get_axis_position_mm(E_AXIS);
    bc_state = 1;
    ui.set_status("送料中…看到噴嘴出料就按「出來了」");
    redraw();
  }
  // Stop feeding: drop the pending chunk and let M410 (quick stop) run in G-code context.
  // The measured length is read once the planner is empty (see bowden_screen).
  static void bc_stop(const bool done) {
    queue.injected_commands[0] = '\0';
    queue.inject("M410\nM82");
    bc_done = done;
    bc_state = 3;
    redraw();
  }
  static void bc_out()    { bc_stop(true); }
  static void bc_cancel() {
    if (bc_state == 1) { bc_stop(false); ui.set_status("已停止送料"); }
    else if (bc_state != 3) { bc_state = 0; ui.goto_screen(filament_screen); }
  }
  static void bc_retry()  { bc_state = 0; redraw(); }
  static void bc_save() {
    #if ENABLED(ADVANCED_PAUSE_FEATURE)
      fc_settings[0].load_length = bc_len;
      fc_settings[0].unload_length = bc_len + 40;
    #endif
    ui.set_status(settings.save() ? "管長已存檔" : "存檔失敗");
    bc_state = 0;
    ui.goto_screen(filament_screen);
  }

  static void bowden_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (bc_state == 1) {
      // Keep a few 5 mm moves queued, slow enough to react to (8 mm/s)
      if (!queue.has_commands_queued() && planner.movesplanned() < 3) queue.inject("M83\nG1 E5 F480");
      if (bc_fed() > 900) { bc_stop(false); ui.set_status("送了 900 mm 還沒出料，已停止"); }
    }
    else if (bc_state == 3 && !queue.has_commands_queued() && !planner.has_blocks_queued()) {
      bc_len = bc_fed();
      bc_state = bc_done ? 2 : 0;
      if (bc_done) ui.set_status("量好了，確認後存檔");
      redraw();
    }
    if (ELAPSED(millis(), next_ms)) { next_ms = millis() + (bc_state == 1 ? 250 : 1000); redraw(); }
    if (!ui.should_draw()) return;

    touch.clear();
    header("管長校正", bc_cancel);
    canvas(0, HEADER_H, TFT_WIDTH, 132, C_BG);
    char buf[72];

    if (bc_state == 0) {
      text(10, 6,  F_S, C_TEXT, C_BG, "1. 先「退料」，把料頭剪平。");
      text(10, 26, F_S, C_TEXT, C_BG, "2. 料插進擠出機，推到剛好被齒輪咬住。");
      text(10, 46, F_S, C_TEXT, C_BG, "3. 按「開始送料」，噴嘴一出料就按「出來了」。");
      sprintf_P(buf, PSTR("目前管長 %d mm"), int(bowden()));
      text(10, 74, F_M, C_MUTED, C_BG, buf);
      if (!noz_ready()) {
        sprintf_P(buf, PSTR("加熱中 %d° / %d°"), thermalManager.wholeDegHotend(0), thermalManager.degTargetHotend(0));
        text(10, 100, F_M, C_WARN, C_BG, buf);
      }
      Btn c; c.label = "取消"; button(4, 170, 100, 66, c, bc_cancel);
      Btn b; b.icon = &qqs_i28_chevrons_down; b.label = "開始送料"; b.enabled = noz_ready();
      b.fg = C_ACCENT; b.border = C_ACCENT;
      button(108, 170, 208, 66, b, bc_start);
    }
    else if (bc_state == 1 || bc_state == 3) {
      text_c(0, 6, TFT_WIDTH, F_S, C_MUTED, C_BG, "已送出");
      sprintf_P(buf, PSTR("%d mm"), int(bc_fed()));
      text_c(0, 26, TFT_WIDTH, F_XL, C_TEXT, C_BG, buf);
      text_c(0, 86, TFT_WIDTH, F_S, C_MUTED, C_BG, "盯著噴嘴，一出料就按右邊");
      Btn c; c.label = "停止"; button(4, 170, 100, 66, c, bc_cancel);
      Btn b; b.icon = &qqs_i28_check; b.label = "出來了！"; b.fg = C_OK; b.border = C_OK;
      button(108, 170, 208, 66, b, bc_out);
    }
    else {
      text_c(0, 6, TFT_WIDTH, F_S, C_MUTED, C_BG, "量到的管長");
      sprintf_P(buf, PSTR("%d mm"), int(bc_len + 0.5f));
      text_c(0, 26, TFT_WIDTH, F_XL, C_TEXT, C_BG, buf);
      sprintf_P(buf, PSTR("之後進料：快送 %d mm，再慢送到出料"), _MAX(0, int(bc_len) - 40));
      text_c(0, 86, TFT_WIDTH, F_S, C_MUTED, C_BG, buf);
      Btn r; r.label = "重來"; button(4, 170, 100, 66, r, bc_retry);
      Btn b; b.icon = &qqs_i28_device_floppy; b.label = "存檔"; b.fg = C_OK; b.border = C_OK;
      button(108, 170, 208, 66, b, bc_save);
    }
  }

  static void fil_bowden() {
    bc_state = 0;
    if (thermalManager.degTargetHotend(0) < 180) set_noz(200);
    ui.goto_screen(bowden_screen);
  }

  static void filament_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (ELAPSED(millis(), next_ms)) { next_ms = millis() + 1000; redraw(); }
    if (!ui.should_draw()) return;

    touch.clear();
    header("進退料", go_home_screen);

    const int16_t tgt = thermalManager.degTargetHotend(0), cur = thermalManager.wholeDegHotend(0);
    const bool ready = noz_ready();
    canvas(4, 36, 114, 44, C_TILE);
    frame(0, 0, 114, 44, ready ? C_OK : (tgt ? C_WARN : C_BORDER));
    text(6, 3, F_S, C_MUTED, C_TILE, ready ? "噴頭 可以進退料" : (tgt ? "噴頭 加熱中" : "噴頭 未加熱"));
    char buf[24];
    sprintf_P(buf, tgt ? PSTR("%d° / %d°") : PSTR("%d°"), cur, tgt);
    text(6, 20, F_M, ready ? C_OK : (tgt ? C_WARN : C_TEXT), C_TILE, buf);

    { Btn b; b.label = "PLA";  b.sub = "200°"; if (tgt == 200) { b.fg = C_ACCENT; b.border = C_ACCENT; } button(122, 36, 96, 44, b, fil_pla); }
    { Btn b; b.label = "PETG"; b.sub = "240°"; if (tgt == 240) { b.fg = C_ACCENT; b.border = C_ACCENT; } button(222, 36, 94, 44, b, fil_petg); }

    { Btn b; b.icon = &qqs_i28_arrow_big_down_lines; b.label = "進料"; b.sub = ready ? nullptr : "加熱後可用"; b.enabled = ready; button(4, 86, 154, 70, b, fil_load); }
    { Btn b; b.icon = &qqs_i28_arrow_big_up_lines;   b.label = "退料"; b.sub = ready ? nullptr : "加熱後可用"; b.enabled = ready; button(162, 86, 154, 70, b, fil_unload); }

    { Btn b; b.icon = &qqs_i18_chevrons_down; b.label = "擠出"; b.sub = "10 mm"; b.enabled = ready; button(4, 162, 102, 74, b, fil_extrude); }
    { Btn b; b.icon = &qqs_i18_chevrons_up;   b.label = "回抽"; b.sub = "10 mm"; b.enabled = ready; button(110, 162, 102, 74, b, fil_retract); }
    sprintf_P(buf, PSTR("%d mm"), int(bowden()));
    { Btn b; b.icon = &qqs_i18_ruler_2; b.label = "管長校正"; b.sub = buf; button(216, 162, 100, 74, b, fil_bowden); }
  }

  // =========================================================================
  // Settings page, machine info, restore defaults
  // =========================================================================

  static void set_save() { ui.set_status(settings.save() ? "已存檔" : "存檔失敗"); redraw(); }
  static void go_settings() { ui.goto_screen(settings_screen); }

  static void info_screen() {
    if (!ui.should_draw()) return;
    touch.clear();
    header("機器資訊", go_settings);
    canvas(0, HEADER_H, TFT_WIDTH, TFT_HEIGHT - HEADER_H, C_BG);
    char v[64], a[16], b[16], c[16];
    int16_t y = 6;
    auto row = [&](const char *label, const char *val) {
      text(10, y, F_S, C_MUTED, C_BG, label);
      text(118, y, F_S, C_TEXT, C_BG, val);
      y += 19;
    };
    row("韌體", "Marlin " QQS_FW_VERSION " (QQS-Pro)");
    #ifdef MKS_WIFI
      if (mks_wifi_net_state == 1) sprintf_P(v, PSTR("%u.%u.%u.%u"), mks_wifi_ip[0], mks_wifi_ip[1], mks_wifi_ip[2], mks_wifi_ip[3]);
      else strcpy(v, "未連線");
      row("Wi-Fi", v);
    #endif
    sprintf_P(v, PSTR("%s mm"), f2(a, delta_height));                                   row("高度 H", v);
    sprintf_P(v, PSTR("X%s  Y%s  Z%s"), f2(a, delta_endstop_adj.a), f2(b, delta_endstop_adj.b), f2(c, delta_endstop_adj.c)); row("塔柱偏移", v);
    sprintf_P(v, PSTR("R %s   L %s"), f2(a, delta_radius), f2(b, delta_diagonal_rod)); row("半徑／連桿", v);
    sprintf_P(v, PSTR("%s mm"), fmt_signed2(qqs_settings.layer_offset));                row("第一層偏移", v);
    sprintf_P(v, PSTR("%s"), f2(a, planner.settings.axis_steps_per_mm[E_AXIS]));       row("E 步數", v);
    sprintf_P(v, PSTR("%d mm"), int(bowden()));                                          row("Bowden 管長", v);
    sprintf_P(v, PSTR("%d mm"), int(DELTA_PRINTABLE_RADIUS));                                  row("列印半徑", v);
    text(10, y + 4, F_S, C_DIM, C_BG, "感測器測試在 v3.2");
  }

  static void reset_confirm() {
    settings.reset();
    ui.set_status(settings.save() ? "已恢復預設並存檔" : "已恢復預設（存檔失敗）");
    ui.return_to_status();
  }
  static void reset_screen() {
    if (!ui.should_draw()) return;
    touch.clear();
    header("恢復預設", go_settings);
    canvas(0, HEADER_H, TFT_WIDTH, 110, C_BG);
    text_c(0, 14, TFT_WIDTH, F_L, C_TEXT, C_BG, "恢復成韌體預設值？");
    text_c(0, 52, TFT_WIDTH, F_S, C_MUTED, C_BG, "校準值、PID、E 步數、管長都會改回預設並存檔。");
    text_c(0, 72, TFT_WIDTH, F_S, C_MUTED, C_BG, "預設的高度與塔柱偏移就是目前校準好的值。");
    Btn c; c.icon = &qqs_i28_x; c.label = "取消"; button(4, 150, 154, 84, c, go_settings);
    Btn s; s.icon = &qqs_i28_restore; s.label = "恢復預設"; s.fg = C_STOP; s.border = C_STOP; s.bg = C_STOPBG;
    button(162, 150, 154, 84, s, reset_confirm);
  }

  static void go_touch_cal() { ui.goto_screen(touch_screen_calibration); }
  static void go_info()      { ui.goto_screen(info_screen); }
  static void go_reset()     { ui.goto_screen(reset_screen); }
  static void go_marlin()    { ui.goto_screen(menu_main); }

  static void settings_screen() {
    if (!ui.should_draw()) return;
    touch.clear();
    header("設定", go_home_screen);
    { Btn b; b.label = "存檔"; b.bg = C_BAR; b.border = C_ACCENT; b.fg = C_ACCENT;
      button(258, 3, 58, HEADER_H - 7, b, set_save); }

    struct Tile { const qqs_bitmap_t *icon; const char *label; const char *sub; screenFunc_t cb; };
    static const Tile tiles[12] = {
      { &qqs_i18_ruler_measure,   "手動校準", "v3.2",   nullptr },
      { &qqs_i18_target,          "自動校準", "需探頭", nullptr },
      { &qqs_i18_grid_dots,       "自動網格", "需探頭", nullptr },
      { &qqs_i18_arrows_vertical, "Z 偏移",   "v3.2",   nullptr },
      { &qqs_i18_chart_line,      "PID 調整", "v3.2",   nullptr },
      { &qqs_i18_adjustments,     "機器參數", "v3.2",   nullptr },
      { &qqs_i18_toggle_right,    "功能開關", "v3.2",   nullptr },
      { &qqs_i18_terminal_2,      "G-code",   "v3.2",   nullptr },
      { &qqs_i18_hand_finger,     "觸控校準", nullptr,  go_touch_cal },
      { &qqs_i18_info_circle,     "機器資訊", nullptr,  go_info },
      { &qqs_i18_restore,         "恢復預設", nullptr,  go_reset },
      { &qqs_i18_list,            "進階選單", "Marlin", go_marlin },
    };
    for (uint8_t i = 0; i < 12; i++) {
      Btn b; b.icon = tiles[i].icon; b.label = tiles[i].label; b.sub = tiles[i].sub;
      b.enabled = tiles[i].cb != nullptr;
      button(4 + (i % 4) * 79, 36 + (i / 4) * 68, 75, 64, b, tiles[i].cb);
    }
  }

  // =========================================================================
  // Status cards (nozzle / bed / fan)
  // =========================================================================

  static void temp_card(int16_t x, int16_t y, int16_t w, int16_t h, bool compact,
                        const qqs_bitmap_t &icon, const char *label, int16_t cur, int16_t target, screenFunc_t cb) {
    const bool heating = target > 0;
    const uint16_t vc = cur >= 50 ? C_HOT : (heating ? C_WARN : C_TEXT);
    canvas(x, y, w, h, C_TILE);
    frame(0, 0, w, h, heating ? C_ACCENT : C_BORDER);
    bitmap(6, 5, icon, heating ? C_ACCENT : C_MUTED, C_TILE);
    text(28, 4, F_S, C_MUTED, C_TILE, label);
    char v[12]; sprintf_P(v, PSTR("%d°"), cur);
    char t[12];
    if (heating) sprintf_P(t, PSTR("→%d°"), target); else strcpy(t, "關");
    if (compact) {
      text_r(w - 6, 4, F_S, heating ? C_ACCENT : C_DIM, C_TILE, t);
      text(8, 22, F_M, vc, C_TILE, v);
    }
    else {
      text(8, 24, F_L, vc, C_TILE, v);
      text_r(w - 6, h - F_S.height - 4, F_S, heating ? C_ACCENT : C_DIM, C_TILE, t);
    }
    touch.add_control(BUTTON, x, y, w, h, (intptr_t)cb);
  }

  static void fan_card(int16_t x, int16_t y, int16_t w, int16_t h, bool compact) {
    const int16_t p = get_fan();
    canvas(x, y, w, h, C_TILE);
    frame(0, 0, w, h, p ? C_COLD : C_BORDER);
    bitmap(6, 5, qqs_i18_propeller, p ? C_COLD : C_MUTED, C_TILE);
    text(28, 4, F_S, C_MUTED, C_TILE, "風扇");
    if (compact) text(8, 22, F_M, p ? C_TEXT : C_MUTED, C_TILE, p ? fmt_int(p, "%") : "關");
    else         text(8, 24, F_L, p ? C_TEXT : C_MUTED, C_TILE, p ? fmt_int(p, "%") : "關");
    touch.add_control(BUTTON, x, y, w, h, (intptr_t)go_fan);
  }

  static void cards(int16_t y, int16_t h, bool compact) {
    temp_card(4,   y, 101, h, compact, qqs_i18_temperature, "噴頭", thermalManager.wholeDegHotend(0), thermalManager.degTargetHotend(0), go_nozzle);
    temp_card(110, y, 101, h, compact, qqs_i18_bed,         "熱床", thermalManager.wholeDegBed(),     thermalManager.degTargetBed(),      go_bed);
    fan_card (216, y, 100, h, compact);
  }

  // =========================================================================
  // Home (idle) screen
  // =========================================================================

  static void home_screen() {
    cards(TOP_H + 4, 58, false);

    struct Tile { const qqs_bitmap_t *icon; const char *label; const char *sub; screenFunc_t cb; };
    static char preheat_sub[12];
    sprintf_P(preheat_sub, PSTR("%d／%d"), PREHEAT_NOZZLE, PREHEAT_BED);
    const Tile tiles[8] = {
      { &qqs_i28_printer,        "列印",   nullptr,     act_print    },
      { &qqs_i28_flame,          "預熱",   preheat_sub, act_preheat  },
      { &qqs_i28_snowflake,      "降溫",   nullptr,     act_cool     },
      { &qqs_i28_home,           "歸零",   nullptr,     act_home     },
      { &qqs_i28_arrows_move,    "移動",   nullptr,     act_move     },
      { &qqs_i28_arrows_down_up, "進退料", nullptr,     act_filament },
      { &qqs_i28_terminal_2,     "G-code", "v3.2",      nullptr      },
      { &qqs_i28_settings,       "設定",   nullptr,     act_settings },
    };
    for (uint8_t i = 0; i < 8; i++) {
      const int16_t col = i % 4, row = i / 4;
      Btn b; b.icon = tiles[i].icon; b.label = tiles[i].label; b.sub = tiles[i].sub;
      b.enabled = tiles[i].cb != nullptr;
      button(4 + col * 79, 90 + row * 75, 75, 71, b, tiles[i].cb);
    }
  }

  // =========================================================================
  // Printing screen
  // =========================================================================

  #if ENABLED(BABYSTEPPING)
    static void bs_step(const float mm) { babystep.add_mm(Z_AXIS, mm); bs_total += mm; redraw(); }
    static void bs_down() { bs_step(-0.02f); }
    static void bs_up()   { bs_step(+0.02f); }
  #endif

  static void act_pause() {
    if (stopping || !card.isFileOpen()) return;
    if (printingIsPaused()) { if (run("M2001")) ui.set_status("回到原位…"); }
    else                    { if (run("M2000")) ui.set_status("停靠中…"); }
    redraw();
  }

  static void printing_screen() {
    cards(TOP_H + 3, 44, true);

    // Progress block
    const int16_t py = TOP_H + 51, ph = 66;
    canvas(0, py, TFT_WIDTH, ph, C_BG);
    const char *name = card.isFileOpen() ? card.longest_filename() : "";
    text(6, 0, F_S, C_MUTED, C_BG, name);
    const uint8_t pct = ui.get_progress_percent();
    text(4, 14, F_XL, C_TEXT, C_BG, fmt_int(pct, "%"));
    char buf[24];
    duration_t elapsed = print_job_timer.duration();
    sprintf_P(buf, PSTR("已印 %s"), fmt_hm(elapsed.value));
    text_r(TFT_WIDTH - 6, 18, F_M, C_TEXT, C_BG, buf);
    #if ENABLED(SHOW_REMAINING_TIME)
      const uint32_t rem = ui.get_remaining_time();
      if (rem) sprintf_P(buf, PSTR("剩餘 %s"), fmt_hm(rem)); else strcpy(buf, "剩餘 --:--");
      text_r(TFT_WIDTH - 6, 40, F_M, C_MUTED, C_BG, buf);
    #endif
    fill(6, ph - 8, TFT_WIDTH - 12, 6, QRGB(0x33, 0x33, 0x33));
    if (pct) fill(6, ph - 8, (TFT_WIDTH - 12) * _MIN(pct, 100) / 100, 6, C_ACCENT);

    // Speed / flow / Z babystep
    const int16_t ry = py + ph + 4;
    {
      Btn b; b.label = "速度"; b.sub = fmt_int(feedrate_percentage, "%");
      if (feedrate_percentage != 100) b.border = C_WARN;
      button(4, ry, 76, 46, b, go_speed);
    }
    {
      char fb[8]; strcpy(fb, fmt_int(planner.flow_percentage[0], "%"));
      Btn b; b.label = "流量"; b.sub = fb;
      if (planner.flow_percentage[0] != 100) b.border = C_WARN;
      button(84, ry, 76, 46, b, go_flow);
    }
    #if ENABLED(BABYSTEPPING)
      {
        Btn m; m.icon = &qqs_i18_minus;  button(164, ry, 44, 46, m, bs_down);
        canvas(208, ry, 64, 46, C_BG);
        text_c(0, 3, 64, F_S, C_MUTED, C_BG, "Z 微調");
        text_c(0, 21, 64, F_M, bs_total == 0 ? C_TEXT : C_WARN, C_BG, fmt_signed2(bs_total));
        Btn p; p.icon = &qqs_i18_plus;   button(272, ry, 44, 46, p, bs_up);
      }
    #endif

    // Pause / stop
    const int16_t by = ry + 50, bh = TFT_HEIGHT - by - 3;
    const bool paused = printingIsPaused();
    {
      Btn b; b.icon = paused ? &qqs_i18_player_play : &qqs_i18_player_pause; b.label = paused ? "繼續" : "暫停";
      b.enabled = !stopping && card.isFileOpen();
      if (paused) { b.fg = C_OK; b.border = C_OK; }
      button(4, by, 154, bh, b, act_pause);
    }
    {
      Btn b; b.icon = &qqs_i18_player_stop; b.label = stopping ? "停止中…" : "停止";
      b.fg = C_STOP; b.border = C_STOP; b.enabled = !stopping;
      button(162, by, 154, bh, b, go_stop);
    }
  }

  // =========================================================================
  // Entry point
  // =========================================================================

  void status_screen() {
    touch.clear();

    const bool job = job_active();
    if (job && !job_was_running) bs_total = 0;   // new job
    job_was_running = job;
    if (!job) stopping = false;

    top_bar();
    if (job || stopping) printing_screen(); else home_screen();
  }

} // namespace qqs

#endif // QQS_UI
