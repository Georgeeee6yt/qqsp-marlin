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
  void anim_draw();              // one frame of the printing animation (also used by M2010)
}
