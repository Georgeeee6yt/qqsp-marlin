/**
 * QQS-Pro v3 UI — helpers shared by the screen files (qqs_ui.cpp, qqs_ui_tools.cpp).
 */
#pragma once

#include "../../../inc/MarlinConfig.h"
#include "../../marlinui.h"
#include "../../../MarlinCore.h"
#include "../../../sd/cardreader.h"
#include "qqs_draw.h"

namespace qqs {
  inline void redraw() { ui.refresh(LCDVIEW_REDRAW_NOW); }
  inline bool job_active() { return printJobOngoing() || card.isFileOpen(); }

  // For screens with live values / background logic: keeps the handler being called and
  // asks for a redraw every `period` ms. Returns whether this call should draw.
  bool live(millis_t &next_ms, const uint16_t period);
  // Inject G-code from the UI (one batch at a time, 64 bytes max). false + status message if busy.
  bool run(const char *g);
  // "12.34" / "-0.50" without printf float support. o: at least 16 bytes.
  char *f2(char *o, const float v);

  void go_settings();
  void go_info();

  // qqs_ui_tools.cpp
  void go_zoff();
  void go_cal();
  void go_sensors();
  void open_bs_prompt(const float total);   // end of a print with Z babystepping: offer to keep it
  void info_tabs(const bool sensors);       // tab strip under the header on the info / sensor pages
  void sensor_poll();                       // from tft_idle: count endstop edges while the sensor page is open
  void go_term();
  void go_pid();
  void go_params();
  void go_toggles();
}
