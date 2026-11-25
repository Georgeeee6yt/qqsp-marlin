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
 *
 * v3.3 probe tools (probe = FIX_MOUNTED + PAUSE_BEFORE_DEPLOY_STOW: the QQS wait screen asks to put it on / take it off).
 * Everything that measures heights runs in the calibration frame (layer offset out of H), so the first-layer
 * offset survives G33 and is not baked into the mesh.
 *   M2007 R<n>            Probe repeatability: n (2..10) probes at the centre -> qqs::pj.z[]
 *   M2007 H               Probe height, step 1: calibration frame, G28, probe the centre (-> pj.measured), stow,
 *                         nozzle to Z1 over the centre. Step 2 is the paper test with M2006 Z; then M851 Z + M2006 S0.
 *   M2007 C P<points>     Delta auto calibration: G33 P<points> in the calibration frame. Old values kept for M2007 U.
 *   M2007 U               Undo the last M2007 C: reload the saved settings (M501: geometry AND the mesh G33 cleared), G28.
 *   M2007 M               Bilinear mesh: G29 in the calibration frame (leveling stays on).
 *   M2007 A P<points>     One go for the magnetic probe (George 2026-11-25): G28, 20 mm down, wait for the probe
 *                         to be put on and tapped (untriggered -> triggered), Z+5 at 3000 mm/s^2 + 50 ms beep,
 *                         3 s countdown (time to take the hand away), G28, G33, G29 with the probe held on (no
 *                         prompts in between), then "take the probe off" and G28. pj.abort cancels the waits.
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

  // ---- v3.3 probe tools (M2007) ----
  #define PRB_MAX 10
  struct ProbeJob {
    volatile bool running, done, ok;
    char kind;                    // 'R' repeatability, 'P' probe height, 'C' delta calibration, 'M' mesh
    uint8_t n, i;                 // repeatability: samples asked / taken
    float z[PRB_MAX];             // repeatability samples (mm)
    float measured;               // 'P': centre height the probe reports (calibration frame)
    millis_t start_ms, end_ms;
    // 'C': values before (actual frame) and after, for the result page and M2007 U
    float oldH, oldR, old_adj[3], old_ang[3], newH, newR, new_adj[3], new_ang[3];
    bool had_mesh;
    // 'A': stage for the UI, countdown, cancel request, results of the two parts
    enum : uint8_t { AS_HOME, AS_WAIT, AS_COUNT, AS_G33, AS_G29, AS_STOW, AS_DONE } ;
    volatile uint8_t stage, count;
    volatile bool abort;
    bool g33_ok, mesh_ok;                // 'C': a mesh existed before (G33 clears it: it no longer fits the new geometry)
    char sd[12];                  // 'C': "0.012" from G33's status line, "" if it did not finish
  };
  extern ProbeJob pj;

  // ---- G29 progress for the mesh picture (filled by G29 through the qqs_g29_* hooks) ----
  struct MeshProgress {
    float lfx, lfy, spx, spy;     // grid start and spacing (mm)
    uint8_t nx, ny;               // grid size; 0 = no G29 has started yet
    uint8_t total, n_done;        // points the probe can reach / measured so far
    int8_t ix, iy;                // point being probed, -1 = none
    uint8_t done[11];             // 1 bit per grid point (index iy * nx + ix), up to 88 points
    millis_t t_first, t_last;     // first and latest point measured
    bool reachable(const uint8_t i, const uint8_t j) const;
    bool is_done(const uint8_t i, const uint8_t j) const { const uint8_t k = j * nx + i; return TEST(done[k >> 3], k & 7); }
  };
  extern MeshProgress mp;
}

// Hooks (gcode/bedlevel/abl/G29.cpp, module/probe.cpp)
void qqs_g29_begin(const float lfx, const float lfy, const float spx, const float spy, const uint8_t nx, const uint8_t ny);
void qqs_g29_at(const uint8_t ix, const uint8_t iy);
void qqs_g29_done(const uint8_t ix, const uint8_t iy);
extern volatile uint16_t qqs_probe_n;   // probe_at_point calls so far (the probe picture "sights" on each)

void qqs_M2003();
void qqs_M2005();
void qqs_M2006();
void qqs_M2007();

#endif
