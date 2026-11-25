/**
 * QQS-Pro: player for the slot animations from qqs_tools/anim2/gen_anim2.py (probe and PID screens).
 * A 160 x 120 base picture plus fixed rectangles ("slots") with a few pre-drawn states each.
 */
#include "../../../inc/MarlinConfig.h"

#if ENABLED(QQS_UI)

#include "qqs_ui_priv.h"

namespace qqs {

  void Anim2::begin(const qqs_anim2_t &an, const int16_t x_, const int16_t y_) {
    a = &an; x = x_; y = y_;
    for (uint8_t i = 0; i < a->nslots; i++) cur[i] = st[i] = a->slots[i].d;
  }

  void Anim2::set(const uint8_t slot, const uint8_t s) {
    if (a && slot < a->nslots && s < a->slots[slot].n) st[slot] = s;
  }

  // Patch drawn into a canvas whose top-left is (ox, oy) in animation coordinates. Level 0 is transparent:
  // clear the rectangle first.
  static void patch(const qqs_anim2_slot_t &sl, const uint8_t s, const int16_t ox, const int16_t oy) {
    const qqs_bitmap_t &b = *sl.f[s];
    fill(sl.x - ox, sl.y - oy, b.w, b.h, C_BG);
    bitmap(sl.x - ox, sl.y - oy, b, C_ACCENT, C_BG);
  }

  void Anim2::full() {
    if (!a) return;
    canvas(x, y, 160, 120, C_BG);
    bitmap(0, 0, *a->base, C_ACCENT, C_BG);
    for (uint8_t i = 0; i < a->nslots; i++) {   // index order: a later slot wins where two overlap
      if (st[i] != a->slots[i].d) patch(a->slots[i], st[i], 0, 0);
      cur[i] = st[i];
    }
  }

  bool Anim2::step() {
    if (!a) return false;
    uint8_t dirty = 0;
    for (uint8_t i = 0; i < a->nslots; i++) if (st[i] != cur[i]) dirty |= _BV(i) | a->slots[i].after;
    if (!dirty) return false;
    for (uint8_t i = 0; i < a->nslots; i++) if (TEST(dirty, i)) {
      const qqs_anim2_slot_t &sl = a->slots[i];
      const qqs_bitmap_t &b = *sl.f[st[i]];
      canvas(x + sl.x, y + sl.y, b.w, b.h, C_BG);
      patch(sl, st[i], sl.x, sl.y);
      cur[i] = st[i];
    }
    return true;
  }

}

#endif // QQS_UI
