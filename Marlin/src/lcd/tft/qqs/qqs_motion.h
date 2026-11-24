/**
 * QQS-Pro v3 — delta reachability helpers and the QQS pause/resume G-codes.
 *
 *   M2000  Pause: retract, raise Z up to +50 (less near the top), park towards 30°
 *          (between the Y and Z towers) at the furthest point reachable at that height.
 *   M2001  Resume: back to XY, then Z, unretract, continue the SD print. No purge, no prompt.
 *   M2002  Before homing after a stop: lift up to 5 mm (clamped to what the carriages allow),
 *          then move to the centre if the straight path is reachable. Used by EVENT_GCODE_SD_ABORT.
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
  // Straight XY move a -> b at a.z stays reachable (13 samples).
  bool segment_ok(const xyz_pos_t &a, const xyz_pos_t &b);
}

void qqs_M2000();
void qqs_M2001();
void qqs_M2002();   // Stop: lift up to 5 mm (never past the reachable top), centre if the path is clear

#if ENABLED(ADVANCED_PAUSE_FEATURE)
  // In pause.cpp (needs its private resume_position)
  void qqs_resume_unpark();
#endif

#endif
