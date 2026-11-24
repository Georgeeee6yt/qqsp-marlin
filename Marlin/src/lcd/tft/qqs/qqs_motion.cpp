/**
 * QQS-Pro v3 — delta reachability helpers and the QQS pause/resume G-codes.
 */
#include "../../../inc/MarlinConfig.h"

#if ENABLED(QQS_UI)

#include "qqs_motion.h"
#include "../../../module/motion.h"
#include "../../../module/delta.h"
#include "../../../module/planner.h"
#include "../../marlinui.h"

#if ENABLED(ADVANCED_PAUSE_FEATURE)
  #include "../../../feature/pause.h"
#endif

namespace qqs {

  // Carriage height of tower i for nozzle position p (same formula as Marlin's DELTA_Z).
  static bool carriage_z(const xyz_pos_t &p, const uint8_t i, float &cz) {
    const float d2 = sq(delta_tower[i].x - p.x) + sq(delta_tower[i].y - p.y),
                l2 = delta_diagonal_rod_2_tower[i];
    if (d2 >= l2) return false;
    cz = p.z + SQRT(l2 - d2);
    return true;
  }

  bool reachable(const xyz_pos_t &p, const float margin) {
    if (p.z < 0 || HYPOT2(p.x, p.y) > sq(float(DELTA_PRINTABLE_RADIUS))) return false;
    const xyz_pos_t home = { 0, 0, delta_height };
    LOOP_ABC(i) {
      float c, top;
      if (!carriage_z(p, i, c) || !carriage_z(home, i, top)) return false;
      if (c > top - margin) return false;
    }
    return true;
  }

  float z_max_at(const float x, const float y) {
    xyz_pos_t p = { x, y, 0 };
    if (!reachable(p)) return -1;
    float lo = 0, hi = delta_height;
    for (uint8_t n = 0; n < 20; n++) {       // bisection, ~0.0003 mm
      p.z = (lo + hi) * 0.5f;
      if (reachable(p)) lo = p.z; else hi = p.z;
    }
    return lo;
  }

  // Furthest reachable radius along `ang` at height z (2 mm steps, 3 mm back-off).
  static float r_max(const float z, const float c, const float s) {
    float best = -1;
    for (float r = 0; r <= DELTA_PRINTABLE_RADIUS; r += 2) {
      const xyz_pos_t p = { r * c, r * s, z };
      if (!reachable(p)) break;
      best = r;
    }
    return best < 0 ? -1 : _MAX(0.0f, best - 3);
  }

  bool segment_ok(const xyz_pos_t &a, const xyz_pos_t &b) {
    for (uint8_t k = 0; k <= 12; k++) {
      const float t = k / 12.0f;
      const xyz_pos_t p = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z };
      if (!reachable(p)) return false;
    }
    return true;
  }

  xyz_pos_t park_point(const xyz_pos_t &from) {
    constexpr float c = 0.8660254f, s = 0.5f;   // 30°: between the Y (330°) and Z (90°) towers
    const float top = z_max_at(from.x, from.y);
    for (float z = _MIN(from.z + 50, top); z > from.z; z -= 5) {
      const float r = r_max(z, c, s);
      if (r < 0) continue;
      const xyz_pos_t start = { from.x, from.y, z }, park = { r * c, r * s, z };
      if (segment_ok(start, park)) return park;
    }
    // Nothing better: stay where we are (Marlin still lifts by NOZZLE_PARK_Z_RAISE_MIN if set)
    return from;
  }

} // namespace qqs

void qqs_M2000() {
  #if ENABLED(ADVANCED_PAUSE_FEATURE)
    if (did_pause_print) return;
    const xyz_pos_t park = qqs::park_point(current_position);
    ui.set_status("停靠中…");
    pause_print(-(PAUSE_PARK_RETRACT_LENGTH), park, false, 0);
    ui.set_status("已暫停，按「繼續」接著印");
  #endif
}

void qqs_M2002() {
  if (axes_should_home()) return;
  planner.synchronize();
  const float top = qqs::z_max_at(current_position.x, current_position.y);
  if (top > current_position.z) do_blocking_move_to_z(_MIN(current_position.z + 5, top), 10);
  const xyz_pos_t centre = { 0, 0, current_position.z };
  if (qqs::reachable(centre) && qqs::segment_ok(current_position, centre)) {
    destination = current_position;
    destination.x = destination.y = 0;
    prepare_internal_move_to_destination(50);
    planner.synchronize();
  }
}

void qqs_M2001() {
  #if ENABLED(ADVANCED_PAUSE_FEATURE)
    if (!did_pause_print) return;
    ui.set_status("回到原位…");
    qqs_resume_unpark();
    ui.set_status("繼續列印");
  #endif
}

#endif // QQS_UI
