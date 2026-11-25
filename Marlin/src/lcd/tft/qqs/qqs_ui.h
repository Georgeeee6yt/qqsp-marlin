/**
 * QQS-Pro v3 UI — screens.
 */
#pragma once

#include <stdint.h>

namespace qqs {
  void boot_screen();
  void boot_progress(uint8_t percent, const char *msg);
  void boot_finish(uint32_t ms_left);   // animate the bar over the remaining boot time
  void status_screen();          // called from MarlinUI::draw_status_screen()
  void anim_idle();              // called from MarlinUI::tft_idle(): printing animation tick
  // qqs_ui_sys.cpp: screens Marlin opens by itself
  void sys_poll();               // from tft_idle: show the "continue" screen when Marlin waits for the user
  void sys_print_done();         // M1001: the next wait is the end of an SD print
  void touch_cal_text(const uint8_t stage, const bool done, const bool ok);
  void kill_text(const char *msg);
  void anim_draw(bool full);     // one frame: full = whole box, else only the moving parts (also used by M2010)
}
