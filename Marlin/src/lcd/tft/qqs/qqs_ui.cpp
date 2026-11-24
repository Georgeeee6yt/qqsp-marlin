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

#if ENABLED(BABYSTEPPING)
  #include "../../../feature/babystep.h"
#endif

#if ENABLED(ADVANCED_PAUSE_FEATURE)
  void menu_change_filament();
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
    if (job_active() || queue.has_commands_queued()) return;
    queue.inject(F("G28"));
    ui.set_status("歸零中…");
    redraw();
  }
  static void act_move()     { ui.goto_screen((screenFunc_t)ui.move_axis_screen); }
  #if ENABLED(ADVANCED_PAUSE_FEATURE)
    static void act_filament() { ui.goto_screen(menu_change_filament); }
  #endif
  static void act_settings() { ui.goto_screen(menu_main); }

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
      { &qqs_i28_arrows_down_up, "進退料", nullptr,     TERN(ADVANCED_PAUSE_FEATURE, act_filament, nullptr) },
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
    if (stopping) return;
    if (printingIsPaused()) { ui.resume_print(); ui.set_status("繼續列印"); }
    else                    { ui.pause_print();  ui.set_status("暫停中…"); }
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
      b.enabled = !stopping;
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
