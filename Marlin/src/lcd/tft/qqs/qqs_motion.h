/**
 * QQS-Pro v3 — delta reachability helpers and the QQS pause/resume G-codes.
 *
 *   M2000  Pause: retract, raise Z up to +50 (less near the top), park towards 30°
 *          (between the Y and Z towers) at the furthest point reachable at that height.
 *   M2001  Resume: back to XY, then Z, unretract, continue the SD print. No purge, no prompt.
 */
#pragma once

#include "../../../inc/MarlinConfig.h"

#if ENABLED(QQS_UI)

namespace qqs {
  // Every carriage stays at least `margin` below its homed (top) position, nozzle inside the printable radius.
  bool reachable(const xyz_pos_t &p, const float margin = 2.0f);
  // Highest Z reachable at this XY (>= 0), or -1 if none.
  float z_max_at(const float x, const float y);
  // Park point for a pause starting from `from`.
  xyz_pos_t park_point(const xyz_pos_t &from);
}

void qqs_M2000();
void qqs_M2001();

#if ENABLED(ADVANCED_PAUSE_FEATURE)
  // In pause.cpp (needs its private resume_position)
  void qqs_resume_unpark();
#endif

#endif
