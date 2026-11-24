/**
 * QQS-Pro v3.2 — calibration / sensor tools that run in G-code context, and the data they share with the UI.
 *
 *   M2003 D<mm> [S] [N]   First-layer offset: + raises the nozzle. Changes M665 H by -D and the stored
 *                         layer offset by +D. If homed (and no N) the nozzle physically follows at once.
 *                         S: save to EEPROM (M500).
 *   M2005                 Endstop repeatability: per tower, down 5 mm and back up at the final homing
 *                         speed (4.5 mm/s) N+1 times; the trigger points go to qqs::rep. Ends with G28.
 *   M2006 S1 / S0         Enter / leave manual calibration: take the layer offset out of H (S1) or put
 *                         it back (S0), then G28.
 *   M2006 P<i>            Go to calibration point i: Z5 travel, then down to Z1.5.
 *   M2006 Z<z>            Move to Z (>= -2). Going up with "one-way approach" on: overshoot 0.2 and come down.
 *   M2006 R<i> Z<z>       Re-home, back to point i at z+1, then slowly down to z.
 *   M2006 A               Apply qqs::cal_res (H, endstop adjustments, radius), then G28.
 */
#pragma once

#include "../../../inc/MarlinConfig.h"

#if ENABLED(QQS_UI)

namespace qqs {

  // ---- Endstop repeatability ----
  #define REP_MAX 10
  struct RepTest {
    volatile bool running, done, abort;
    uint8_t n, tower, sample;
    float pos[3][REP_MAX];        // trigger position relative to the first (seed) trigger, mm
  };
  extern RepTest rep;
  float rep_range(const uint8_t t);
  float rep_drift(const uint8_t t);   // fitted change from first to last sample, mm

  // ---- Manual delta calibration ----
  #define CAL_MAX_PTS 7
  extern const float cal_pts[CAL_MAX_PTS][2];
  struct CalResult {
    float H, adj[3], R;           // new M665 H / M666 XYZ / M665 R (H in the calibration frame: no layer offset)
    float resid[CAL_MAX_PTS];     // predicted error at each point with the new values
    float resid_max;
  };
  extern CalResult cal_res;
  extern bool cal_active;         // M2006 S1 took the layer offset out of H
  // z[i]: where the paper dragged at point i (calibration frame). Uses the current M665/M666 as the start.
  bool cal_solve(const float z[], const uint8_t n);
}

void qqs_M2003();
void qqs_M2005();
void qqs_M2006();

#endif
