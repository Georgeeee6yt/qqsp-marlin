/**
 * QQS-Pro v3 UI — helpers shared by the screen files (qqs_ui.cpp, qqs_ui_tools.cpp).
 */
#pragma once

#include "../../../inc/MarlinConfig.h"
#include "../../marlinui.h"
#include "../../../MarlinCore.h"
#include "../../../sd/cardreader.h"
#include "qqs_draw.h"
#include "qqs_anim2_data.h"

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
  void info_tabs(const uint8_t tab);        // tab strip under the header on the info pages: 0 info, 1 sensors, 2 Wi-Fi
  void go_wifi();
  // Save button with its own feedback (pages without a status line)
  void save_feedback(const bool ok);
  void save_feedback_poll();
  void save_button(const int16_t x, const int16_t y, const int16_t w, const int16_t h, screenFunc_t cb, const bool enabled = true);
  void sensor_poll();                       // from tft_idle: count endstop edges while the sensor page is open
  void go_term();
  void go_pid();
  void go_params();
  void go_toggles();
  // qqs_ui_probe.cpp (v3.3)
  void go_probe();
  void go_mesh();

  // qqs_anim2.cpp: the probe / PID animations (base picture + slots, see qqs_tools/anim2/gen_anim2.py)
  struct Anim2 {
    const qqs_anim2_t *a = nullptr;
    int16_t x = 0, y = 0;
    uint8_t cur[8], st[8];                // drawn / wanted state per slot
    void begin(const qqs_anim2_t &an, const int16_t x_, const int16_t y_);   // every slot to its base state
    void set(const uint8_t slot, const uint8_t s);
    void full();                          // the whole 160 x 120 picture, in its own canvas
    bool step();                          // only the slots that changed (each its own canvas); false: nothing to do
  };
  // From tft_idle: animation frames and the once-a-second values on the probe job / PID screens
  void probe_anim_idle();
  void pid_anim_idle();
}
