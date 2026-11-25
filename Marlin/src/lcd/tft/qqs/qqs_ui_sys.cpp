/**
 * QQS-Pro v3.2.8 UI — the screens Marlin opens by itself, in the QQS style (Marlin's own menus are no
 * longer built): filament change / pause messages (M600, M125, M701, M702, runout), the generic
 * "waiting for the user" screen (M0 / M1 / host prompts), the power-loss resume prompt (M1000 S),
 * the EEPROM mismatch prompt, the texts of the touch calibration and the kill screen.
 *
 * The flow logic stays Marlin's (feature/pause.cpp etc.): these screens only show the state and answer
 * the way Marlin's menus did — clear wait_for_user, or set pause_menu_response.
 *
 * Every Chinese string here is collected by qqs_tools/gen_assets.py (re-run it after text changes).
 */
#include "../../../inc/MarlinConfig.h"

#if ENABLED(QQS_UI)

#include "qqs_ui_priv.h"
#include "../tft.h"
#include "../touch.h"
#include "../../../module/temperature.h"
#include "../../../module/settings.h"
#include "../../../gcode/queue.h"
#include "../../../module/printcounter.h"
#if ENABLED(ADVANCED_PAUSE_FEATURE)
  #include "../../../feature/pause.h"
#endif
#if HAS_FILAMENT_SENSOR
  #include "../../../feature/runout.h"
#endif
#if ENABLED(POWER_LOSS_RECOVERY)
  #include "../../../feature/powerloss.h"
#endif
#if HAS_BUZZER
  #include "../../../libs/buzzer.h"
#endif

extern bool wait_for_user;
#if ENABLED(TOUCH_SCREEN_CALIBRATION)
  void touch_screen_calibration();   // menu_touch_screen.cpp (settings tile, long press)
#endif

namespace qqs {

  // Title bar without a back button (these screens are left by answering them)
  static void bar(const char *title) {
    canvas(0, 0, TFT_WIDTH, HEADER_H, C_BAR);
    text_c(0, (HEADER_H - F_M.height) / 2, TFT_WIDTH, F_M, C_TEXT, C_BAR, title);
    fill(0, HEADER_H - 1, TFT_WIDTH, 1, C_BORDER);
  }

  // Own canvas (absolute y), so a temperature change redraws only this line
  static void nozzle_line(const int16_t y) {
    canvas(0, y, TFT_WIDTH, 22, C_BG);
    char v[32];
    const int16_t t = thermalManager.degTargetHotend(0);
    if (t) sprintf_P(v, PSTR("噴頭 %d°／%d°"), thermalManager.wholeDegHotend(0), t);
    else   sprintf_P(v, PSTR("噴頭 %d°（關）"), thermalManager.wholeDegHotend(0));
    text_c(0, 0, TFT_WIDTH, F_M, t && thermalManager.wholeDegHotend(0) + 5 < t ? C_WARN : C_TEXT, C_BG, v);
  }
  static uint16_t temp_sig() { return uint16_t((thermalManager.wholeDegHotend(0) & 0x1FF) | (uint16_t(thermalManager.degTargetHotend(0) > 0) << 15)); }

  static void user_go() { wait_for_user = false; redraw(); }

  // =========================================================================
  // Filament change / pause messages (MarlinUI::pause_show_message below)
  // =========================================================================

  #if ENABLED(ADVANCED_PAUSE_FEATURE)

    static PauseMessage pz_msg = PAUSE_MESSAGE_STATUS;
    static uint32_t pz_sig = 0xFFFFFFFF;

    static void pz_more()   { pause_menu_response = PAUSE_RESPONSE_EXTRUDE_MORE; redraw(); }
    static void pz_resume() { pause_menu_response = PAUSE_RESPONSE_RESUME_PRINT; redraw(); }
    #if HAS_FILAMENT_SENSOR
      static void pz_sensor_off() { runout.enabled = false; runout.reset(); pz_sig = 0xFFFFFFFF; redraw(); }
    #endif

    static void pause_screen() {
      static millis_t next_ms = 0;
      ui.defer_status_screen();
      const bool draw = live(next_ms, 500);
      #if HAS_FILAMENT_SENSOR
        const bool still_out = pz_msg == PAUSE_MESSAGE_OPTION && runout.filament_ran_out;
      #else
        constexpr bool still_out = false;
      #endif
      const uint32_t sig = (uint32_t(pz_msg) << 24) | (uint32_t(wait_for_user) << 23) | (uint32_t(still_out) << 22)
                         | (uint32_t(thermalManager.degTargetHotend(0) & 0x3FF) << 10);
      static uint16_t tsig = 0xFFFF;
      if (!draw) return;
      if (sig == pz_sig) {                       // same state: only the temperature line
        if (temp_sig() != tsig) { tsig = temp_sig(); nozzle_line(HEADER_H + 92); }
        return;
      }
      pz_sig = sig; tsig = temp_sig();

      const char *title;
      switch (pause_mode) {
        case PAUSE_MODE_LOAD_FILAMENT:   title = "進料"; break;
        case PAUSE_MODE_UNLOAD_FILAMENT: title = "退料"; break;
        case PAUSE_MODE_PAUSE_PRINT:     title = "暫停"; break;
        default:                         title = "換料"; break;
      }
      const char *msg = "", *hint = nullptr, *btn = nullptr;
      switch (pz_msg) {
        case PAUSE_MESSAGE_PARKING:  msg = "移到停放位置…"; break;
        case PAUSE_MESSAGE_CHANGING: msg = "準備換料…"; break;
        case PAUSE_MESSAGE_UNLOAD:   msg = "退料中…"; break;
        case PAUSE_MESSAGE_WAITING:  msg = "已暫停"; hint = "按「繼續」接著印"; btn = "繼續"; break;
        case PAUSE_MESSAGE_INSERT:   msg = "請放入新的料"; hint = "料頭插到擠出機，再按「繼續」"; btn = "繼續"; break;
        case PAUSE_MESSAGE_LOAD:     msg = "進料中…"; break;
        case PAUSE_MESSAGE_PURGE:    msg = "擠出清料中…"; hint = "出來的顏色對了可以先停"; btn = "停止擠出"; break;
        case PAUSE_MESSAGE_RESUME:   msg = "準備繼續列印…"; break;
        case PAUSE_MESSAGE_HEAT:     msg = "噴頭已降溫"; hint = "按「加熱」回到原本的溫度"; btn = "加熱"; break;
        case PAUSE_MESSAGE_HEATING:  msg = "噴頭加熱中，請稍候…"; break;
        case PAUSE_MESSAGE_OPTION:   msg = still_out ? "感測器還是顯示沒料" : "要再擠一點，還是繼續？";
                                     hint = still_out ? "沒有斷料感測器就把它關掉" : nullptr; break;
        default: break;
      }

      touch.clear();
      bar(title);
      canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
      text_c(0, 22, TFT_WIDTH, F_M, C_TEXT, C_BG, msg);
      if (hint) text_c(0, 52, TFT_WIDTH, F_S, C_MUTED, C_BG, hint);
      nozzle_line(HEADER_H + 92);

      if (pz_msg == PAUSE_MESSAGE_OPTION) {
        Btn m; m.icon = &qqs_i18_arrow_big_down_lines; m.label = "再擠一點";
        button(4, 180, 154, 56, m, pz_more);
        #if HAS_FILAMENT_SENSOR
          if (still_out) {
            Btn s; s.icon = &qqs_i18_toggle_right; s.label = "關閉感測器"; s.fg = C_WARN; s.border = C_WARN;
            button(162, 180, 154, 56, s, pz_sensor_off);
            return;
          }
        #endif
        Btn r; r.icon = &qqs_i18_player_play; r.label = "繼續"; r.fg = C_ACCENT; r.border = C_ACCENT;
        button(162, 180, 154, 56, r, pz_resume);
      }
      else if (btn && wait_for_user) {
        Btn b; b.icon = pz_msg == PAUSE_MESSAGE_HEAT ? &qqs_i18_flame : pz_msg == PAUSE_MESSAGE_PURGE ? &qqs_i18_player_stop : &qqs_i18_player_play;
        b.label = btn; b.fg = C_ACCENT; b.border = C_ACCENT;
        button(4, 180, 312, 56, b, user_go);
      }
      else canvas(0, 180, TFT_WIDTH, 56, C_BG);
    }

    void pause_show(const uint8_t message) {
      pz_msg = PauseMessage(message);
      if (pz_msg == PAUSE_MESSAGE_OPTION) pause_menu_response = PAUSE_RESPONSE_WAIT_FOR;
      if (pz_msg == PAUSE_MESSAGE_STATUS) { ui.return_to_status(); return; }
      pz_sig = 0xFFFFFFFF;
      ui.defer_status_screen();
      if (ui.currentScreen != pause_screen) ui.goto_screen(pause_screen); else redraw();
    }

    static bool in_pause_screen() { return ui.currentScreen == pause_screen; }

  #else
    static bool in_pause_screen() { return false; }
  #endif // ADVANCED_PAUSE_FEATURE

  // =========================================================================
  // Waiting for the user outside the pause screens: M0 / M1, probe deploy / stow (PAUSE_BEFORE_DEPLOY_STOW),
  // end of an SD print (M1001 with PRINTER_EVENT_LEDS waits up to PE_LEDS_COMPLETED_TIME).
  // Leaves to the screen it interrupted once wait_for_user is cleared (button, M108, timeout).
  // =========================================================================

  static bool ws_drawn = false, ws_done = false;
  static screenFunc_t ws_prev = nullptr;

  void sys_print_done() { ws_done = true; }   // M1001, just before it waits

  // Whole UTF-8 characters of s that fit in maxw (with "…" when cut); o: 64 bytes
  static void fit_msg(char *o, const char *s, const qqs_font_t &f, const uint16_t maxw) {
    uint8_t n = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p;) {
      const uint8_t *q = p; qqs_utf8_next(q);
      const uint8_t l = q - p;
      if (n + l > 60) break;
      memcpy(o + n, p, l); n += l; p = q;
    }
    o[n] = 0;
    if (qqs_text_width(&f, o) <= maxw) return;
    const uint16_t ew = qqs_text_width(&f, "…");
    while (n && qqs_text_width(&f, o) + ew > maxw) { do --n; while (n && (uint8_t(o[n]) & 0xC0) == 0x80); o[n] = 0; }
    strcat(o, "…");
  }

  static void ws_go() { wait_for_user = false; }

  static void wait_screen() {
    static millis_t next_ms = 0;
    static uint16_t tsig = 0xFFFF;
    ui.defer_status_screen();
    if (!wait_for_user) {
      ws_done = false;
      screenFunc_t back = ws_prev; ws_prev = nullptr;
      if (back) ui.goto_screen(back); else ui.return_to_status();
      return;
    }
    if (!live(next_ms, 1000)) return;
    if (ws_drawn) { if (!ws_done && temp_sig() != tsig) { tsig = temp_sig(); nozzle_line(HEADER_H + 100); } return; }
    ws_drawn = true; tsig = temp_sig();
    touch.clear();

    char v[64];
    if (ws_done) {
      bar("列印完成");
      canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
      text_c(0, 30, TFT_WIDTH, F_M, C_OK, C_BG, "列印完成");
      const uint32_t t = print_job_timer.duration();
      if (t >= 3600) sprintf_P(v, PSTR("用時 %u 小時 %u 分"), unsigned(t / 3600), unsigned((t / 60) % 60));
      else           sprintf_P(v, PSTR("用時 %u 分 %u 秒"), unsigned(t / 60), unsigned(t % 60));
      text_c(0, 64, TFT_WIDTH, F_S, C_MUTED, C_BG, v);
      text_c(0, 100, TFT_WIDTH, F_S, C_DIM, C_BG, "噴頭降溫前不要碰列印件");
      Btn b; b.icon = &qqs_i18_check; b.label = "好"; b.fg = C_ACCENT; b.border = C_ACCENT;
      button(4, 180, 312, 56, b, ws_go);
      return;
    }

    // Marlin's own messages (language_en) -> Chinese; anything else (M0 text) shown as given
    const char *m = ui.status_message;
    const char *hint = "按「繼續」接著執行";
    if (!m || !*m || !strcmp_P(m, GET_TEXT(MSG_USERWAIT))) m = "G-code 要求暫停";
    #if ENABLED(PAUSE_BEFORE_DEPLOY_STOW)
      else if (!strcmp_P(m, GET_TEXT(MSG_MANUAL_DEPLOY))) { m = "請裝上探頭"; hint = "裝好再按「繼續」"; }
      else if (!strcmp_P(m, GET_TEXT(MSG_MANUAL_STOW)))   { m = "請取下探頭"; hint = "取下再按「繼續」"; }
    #endif
    bar("等待中");
    canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
    fit_msg(v, m, F_M, TFT_WIDTH - 16);
    text_c(0, 30, TFT_WIDTH, F_M, C_TEXT, C_BG, v);
    text_c(0, 64, TFT_WIDTH, F_S, C_MUTED, C_BG, hint);
    nozzle_line(HEADER_H + 100);
    Btn b; b.icon = &qqs_i18_player_play; b.label = "繼續"; b.fg = C_ACCENT; b.border = C_ACCENT;
    button(4, 180, 312, 56, b, ws_go);
  }

  // From tft_idle: Marlin is waiting for a click and no screen of ours asks for it -> show one
  void sys_poll() {
    if (!wait_for_user || in_pause_screen() || ui.currentScreen == wait_screen) return;
    #if ENABLED(TOUCH_SCREEN_CALIBRATION)
      if (ui.currentScreen == MarlinUI::touch_calibration_screen || ui.currentScreen == touch_screen_calibration) return;
    #endif
    ws_prev = ui.on_status_screen() ? nullptr : ui.currentScreen;
    ws_drawn = false;
    ui.goto_screen(wait_screen);
  }

  // =========================================================================
  // Power-loss recovery (M1000 S at boot)
  // =========================================================================

  #if ENABLED(POWER_LOSS_RECOVERY)

    static void rec_resume() { ui.return_to_status(); queue.inject(F("M1000")); }
    static void rec_cancel() { recovery.cancel(); ui.return_to_status(); }

    static void recovery_screen() {
      static millis_t next_ms = 0;
      ui.defer_status_screen();
      if (!live(next_ms, 60000)) return;
      touch.clear();
      bar("斷電續印");
      canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
      text_c(0, 18, TFT_WIDTH, F_M, C_TEXT, C_BG, "上次列印中途斷電");
      char v[40]; const char *f = recovery.info.sd_filename;
      uint8_t n = 0;
      for (; f[n] && n < 36; n++) v[n] = (f[n] < 0x20 || f[n] > 0x7E) ? '?' : f[n];
      v[n] = 0;
      text_c(0, 48, TFT_WIDTH, F_S, C_MUTED, C_BG, v);
      text_c(0, 80, TFT_WIDTH, F_S, C_MUTED, C_BG, "要從中斷的地方接著印嗎？");
      text_c(0, 98, TFT_WIDTH, F_S, C_DIM, C_BG, "熱床上的列印件不要移動。");
      Btn c; c.icon = &qqs_i18_x; c.label = "取消";
      button(4, 180, 154, 56, c, rec_cancel);
      Btn r; r.icon = &qqs_i18_player_play; r.label = "繼續列印"; r.fg = C_ACCENT; r.border = C_ACCENT;
      button(162, 180, 154, 56, r, rec_resume);
    }

    void go_recovery() { ui.goto_screen(recovery_screen); }

  #endif

  // =========================================================================
  // EEPROM mismatch at boot (EEPROM_AUTO_INIT is off): settings are the firmware defaults in RAM
  // =========================================================================

  static uint8_t ee_err = 0;
  static void ee_save() {
    const bool ok = settings.init_eeprom();
    ui.set_status(ok ? "已存成預設" : "存檔失敗");
    ui.return_to_status();
  }
  static void ee_skip() { ui.return_to_status(); }

  static void eeprom_screen() {
    static millis_t next_ms = 0;
    ui.defer_status_screen();
    if (!live(next_ms, 60000)) return;
    touch.clear();
    bar("EEPROM 資料不符");
    canvas(0, HEADER_H, TFT_WIDTH, 146, C_BG);
    static const char * const why[5] = { "", "版本不符", "大小不符", "校驗碼錯誤", "資料損壞" };
    char v[48];
    sprintf_P(v, PSTR("存的設定讀不進來（%s）"), why[ee_err < 5 ? ee_err : 0]);
    text_c(0, 14, TFT_WIDTH, F_M, C_WARN, C_BG, v);
    text_c(0, 46, TFT_WIDTH, F_S, C_TEXT, C_BG, "韌體更新後設定格式變了，現在用的是韌體預設值。");
    text_c(0, 66, TFT_WIDTH, F_S, C_MUTED, C_BG, "按「存成預設」寫入，之後就不會再問；");
    text_c(0, 84, TFT_WIDTH, F_S, C_MUTED, C_BG, "校準、PID、E 步數要再做一次。");
    Btn s; s.icon = &qqs_i18_x; s.label = "先不存";
    button(4, 180, 154, 56, s, ee_skip);
    Btn r; r.icon = &qqs_i18_device_floppy; r.label = "存成預設"; r.fg = C_ACCENT; r.border = C_ACCENT;
    button(162, 180, 154, 56, r, ee_save);
  }

  void eeprom_alert(const uint8_t err) { ee_err = err; ui.goto_screen(eeprom_screen); }

  // =========================================================================
  // Touch calibration text (the crosses and the logic stay Marlin's, ui_common.cpp)
  // =========================================================================

  void touch_cal_text(const uint8_t stage, const bool done, const bool ok) {
    static const char * const where[4] = { "左上", "左下", "右上", "右下" };
    char v[48];
    const int16_t y = (TFT_HEIGHT - 40) / 2;
    canvas(40, y, TFT_WIDTH - 80, 40, C_BG);
    if (done) {
      text_c(0, 0, TFT_WIDTH - 80, F_M, ok ? C_OK : C_STOP, C_BG, ok ? "觸控校準完成" : "觸控校準失敗");
      text_c(0, 22, TFT_WIDTH - 80, F_S, C_MUTED, C_BG, "點一下回去");
    }
    else {
      sprintf_P(v, PSTR("點一下%s的十字中心"), where[stage < 4 ? stage : 0]);
      text_c(0, 0, TFT_WIDTH - 80, F_M, C_TEXT, C_BG, v);
      text_c(0, 22, TFT_WIDTH - 80, F_S, C_MUTED, C_BG, "用指甲或觸控筆比較準");
    }
  }

  // =========================================================================
  // Kill screen (thermal runaway, MAXTEMP ...): drawn once, Marlin stops after it
  // =========================================================================

  void kill_text(const char *msg) {
    canvas(0, 40, TFT_WIDTH, 150, C_STOPBG);
    text_c(0, 0, TFT_WIDTH, F_M, C_TEXT, C_STOPBG, "印表機已停止");
    text_c(0, 40, TFT_WIDTH, F_M, C_WARN, C_STOPBG, (msg && *msg) ? msg : "錯誤");
    text_c(0, 90, TFT_WIDTH, F_S, C_TEXT, C_STOPBG, "加熱已全部關閉。");
    text_c(0, 110, TFT_WIDTH, F_S, C_TEXT, C_STOPBG, "請關機檢查後再重新開機。");
  }

} // namespace qqs

#if ENABLED(ADVANCED_PAUSE_FEATURE)
  void MarlinUI::pause_show_message(const PauseMessage message, const PauseMode mode/*=PAUSE_MODE_SAME*/, const uint8_t extruder/*=active_extruder*/) {
    if (mode != PAUSE_MODE_SAME) pause_mode = mode;
    UNUSED(extruder);
    qqs::pause_show(message);
  }
#endif

#endif // QQS_UI
