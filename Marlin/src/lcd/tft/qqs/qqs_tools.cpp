/**
 * QQS-Pro v3.2 — Z offset, endstop repeatability and manual delta calibration (G-code side).
 * See qqs_tools.h for the commands.
 */
#include "../../../inc/MarlinConfig.h"

#if ENABLED(QQS_UI)

#include "qqs_tools.h"
#include "qqs_settings.h"
#include "../../../module/motion.h"
#include "../../../module/delta.h"
#include "../../../module/planner.h"
#include "../../../module/stepper.h"
#include "../../../module/endstops.h"
#include "../../../module/settings.h"
#include "../../../gcode/gcode.h"
#include "../../../gcode/parser.h"
#include "../../../module/probe.h"
#include "../../../MarlinCore.h"
#if HAS_BUZZER
  #include "../../../libs/buzzer.h"
#endif
#include "../../../feature/bedlevel/bedlevel.h"
#include "../../marlinui.h"
#if HAS_BED_PROBE
  #include "../../../module/probe.h"
#endif

// motion.cpp (not in motion.h)
void do_homing_move(const AxisEnum axis, const float distance, const feedRate_t fr_mm_s, const bool final_approach);

namespace qqs {

  RepTest rep;
  CalResult cal_res;
  bool cal_active = false;

  // Centre, the three towers at r 100, then between the towers (7-point mode only).
  const float cal_pts[CAL_MAX_PTS][2] = {
    {   0.0f,   0.0f },   // 0 centre
    { -86.6f, -50.0f },   // 1 X tower
    {  86.6f, -50.0f },   // 2 Y tower
    {   0.0f, 100.0f },   // 3 Z tower
    {   0.0f,-100.0f },   // 4 between X and Y
    {  86.6f,  50.0f },   // 5 between Y and Z
    { -86.6f,  50.0f },   // 6 between Z and X
  };

  // =========================================================================
  // Endstop repeatability
  // =========================================================================

  float rep_range(const uint8_t t) {
    float lo = rep.pos[t][0], hi = lo;
    for (uint8_t k = 1; k < rep.n; k++) { NOLESS(hi, rep.pos[t][k]); NOMORE(lo, rep.pos[t][k]); }
    return hi - lo;
  }

  float rep_drift(const uint8_t t) {     // least-squares slope * (n - 1)
    const uint8_t n = rep.n;
    if (n < 2) return 0;
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint8_t k = 0; k < n; k++) { sx += k; sy += rep.pos[t][k]; sxx += k * k; sxy += k * rep.pos[t][k]; }
    const float den = n * sxx - sx * sx;
    return den ? (n * sxy - sx * sy) / den * (n - 1) : 0;
  }

  // =========================================================================
  // Delta kinematics in double, for the calibration solve
  // =========================================================================

  struct DParams { double H, adj[3], R; };

  static void towers(const double R, double tx[3], double ty[3]) {
    static const double base[3] = { 210, 330, 90 };
    LOOP_ABC(i) {
      const double a = RADIANS(base[i] + delta_tower_angle_trim[i]);
      tx[i] = R * cos(a); ty[i] = R * sin(a);
    }
  }

  static double rod2(const uint8_t i) { const double l = delta_diagonal_rod + delta_diagonal_rod_trim[i]; return l * l; }

  // Carriage heights (in the frame set up by homing) for a nozzle position.
  static void ik(const double x, const double y, const double z, const double R, double c[3]) {
    double tx[3], ty[3]; towers(R, tx, ty);
    LOOP_ABC(i) c[i] = z + sqrt(rod2(i) - sq(x - tx[i]) - sq(y - ty[i]));
  }

  // Nozzle Z for carriage heights c (trilateration, lower intersection).
  static double fk_z(const double c[3], const double R) {
    double tx[3], ty[3]; towers(R, tx, ty);
    const double p1[3] = { tx[0], ty[0], c[0] };
    double ex[3] = { tx[1] - p1[0], ty[1] - p1[1], c[1] - p1[2] };
    const double d = sqrt(sq(ex[0]) + sq(ex[1]) + sq(ex[2]));
    LOOP_L_N(k, 3) ex[k] /= d;
    const double p13[3] = { tx[2] - p1[0], ty[2] - p1[1], c[2] - p1[2] };
    const double i = ex[0] * p13[0] + ex[1] * p13[1] + ex[2] * p13[2];
    double ey[3] = { p13[0] - i * ex[0], p13[1] - i * ex[1], p13[2] - i * ex[2] };
    const double j = sqrt(sq(ey[0]) + sq(ey[1]) + sq(ey[2]));
    LOOP_L_N(k, 3) ey[k] /= j;
    const double ez[3] = { ex[1] * ey[2] - ex[2] * ey[1], ex[2] * ey[0] - ex[0] * ey[2], ex[0] * ey[1] - ex[1] * ey[0] };
    const double r1 = rod2(0), r2 = rod2(1), r3 = rod2(2);
    const double X = (r1 - r2 + d * d) / (2 * d),
                 Y = (r1 - r3 + i * i + j * j) / (2 * j) - (i / j) * X,
                 Z = -sqrt(_MAX(0.0, r1 - X * X - Y * Y));
    return p1[2] + X * ex[2] + Y * ey[2] + Z * ez[2];
  }

  // Homed carriage height (frame) = H + sqrt(L^2 - R^2); physical carriage = endstop + adj.
  // A touch measured at (x, y, z) with params p0 is re-evaluated with params p.
  static double z_with(const double x, const double y, const double z, const DParams &p0, const DParams &p) {
    double c0[3], cnew[3], top0[3], top[3];
    ik(x, y, z, p0.R, c0);
    ik(0, 0, p0.H, p0.R, top0);
    ik(0, 0, p.H, p.R, top);
    LOOP_ABC(i) cnew[i] = c0[i] - top0[i] + p0.adj[i] - p.adj[i] + top[i];
    return fk_z(cnew, p.R);
  }

  static float probe_z() { return TERN0(HAS_BED_PROBE, probe.offset.z); }

  bool cal_solve(const float z[], const uint8_t n) {
    DParams p0;
    p0.H = delta_height - probe_z();
    LOOP_ABC(i) p0.adj[i] = delta_endstop_adj[i];
    p0.R = delta_radius;

    // Unknowns: H, adj X, adj Y, R (adj Z held; the common part of the three adj is the same as H)
    DParams p = p0;
    double r[CAL_MAX_PTS], J[4][CAL_MAX_PTS];
    auto eval = [&](const DParams &q, double *out) {
      LOOP_L_N(k, n) out[k] = z_with(cal_pts[k][0], cal_pts[k][1], z[k], p0, q);
    };
    LOOP_L_N(iter, 6) {
      eval(p, r);
      LOOP_L_N(u, 4) {
        DParams q = p;
        const double h = 1e-4;
        switch (u) { case 0: q.H += h; break; case 1: q.adj[0] += h; break; case 2: q.adj[1] += h; break; default: q.R += h; }
        double rr[CAL_MAX_PTS];
        eval(q, rr);
        LOOP_L_N(k, n) J[u][k] = (rr[k] - r[k]) / h;
      }
      // Normal equations (J^T J) dq = -J^T r, Gauss-Jordan with partial pivoting
      double M[4][5];
      LOOP_L_N(a, 4) {
        LOOP_L_N(b, 4) { double s = 0; LOOP_L_N(k, n) s += J[a][k] * J[b][k]; M[a][b] = s; }
        double g = 0; LOOP_L_N(k, n) g -= J[a][k] * r[k]; M[a][4] = g;
      }
      LOOP_L_N(c, 4) {
        uint8_t piv = c;
        for (uint8_t q = c + 1; q < 4; q++) if (ABS(M[q][c]) > ABS(M[piv][c])) piv = q;
        if (ABS(M[piv][c]) < 1e-12) return false;
        if (piv != c) LOOP_L_N(k, 5) { const double t = M[c][k]; M[c][k] = M[piv][k]; M[piv][k] = t; }
        LOOP_L_N(q, 4) if (q != c) {
          const double f = M[q][c] / M[c][c];
          LOOP_L_N(k, 5) M[q][k] -= f * M[c][k];
        }
      }
      p.H += M[0][4] / M[0][0];
      p.adj[0] += M[1][4] / M[1][1];
      p.adj[1] += M[2][4] / M[2][2];
      p.R += M[3][4] / M[3][3];
    }

    eval(p, r);
    cal_res.resid_max = 0;
    LOOP_L_N(k, n) { cal_res.resid[k] = r[k]; NOLESS(cal_res.resid_max, float(ABS(r[k]))); }

    // Normalise: highest adjustment = 0 (moving all three by k is the same as moving H by k)
    const double k = _MAX(p.adj[0], _MAX(p.adj[1], p.adj[2]));
    LOOP_ABC(i) cal_res.adj[i] = p.adj[i] - k;
    cal_res.H = p.H - k + probe_z();
    cal_res.R = p.R;
    // Sanity: refuse wild answers (bad measurements)
    if (ABS(cal_res.H - delta_height) > 5 || ABS(cal_res.R - delta_radius) > 5) return false;
    LOOP_ABC(i) if (cal_res.adj[i] < -5) return false;
    return true;
  }

} // namespace qqs

using namespace qqs;

// ===========================================================================
// M2003: first-layer offset
// ===========================================================================

void qqs_M2003() {
  const float d = parser.floatval('D');
  const bool save = parser.seen_test('S'), nomove = parser.seen_test('N');
  planner.synchronize();
  qqs_settings.layer_offset += d;
  delta_height -= d;
  recalc_delta_settings();
  // Homing sets up the carriage frame from H, so redo that shift by hand: the same commanded Z
  // is now D higher. Re-declare where we are (no motion), then go back to the commanded Z.
  if (d && !nomove && all_axes_homed()) {
    const float z = current_position.z;
    current_position.z -= d;
    sync_plan_position();
    do_blocking_move_to_z(z, 5);
  }
  if (save) settings.save();
}

// ===========================================================================
// M2005: endstop repeatability
// ===========================================================================

void qqs_M2005() {
  rep.n = qqs_settings.test_count == 10 ? 10 : 5;
  rep.abort = false; rep.done = false; rep.running = true;
  LOOP_ABC(t) LOOP_L_N(k, REP_MAX) rep.pos[t][k] = 0;

  planner.synchronize();
  gcode.process_subcommands_now(F("G28"));

  constexpr feedRate_t fr_down = 20, fr_up = 4.5f;   // 4.5 = final homing approach (45 / bump divisor 10)
  endstops.enable(true);
  LOOP_ABC(t) {
    if (rep.abort) break;
    rep.tower = t;
    const AxisEnum ax = AxisEnum(t);
    float at = 0;
    // k = 0 is the seed bump (from the homed position, which is the M666 offset below the switch): not recorded.
    for (uint8_t k = 0; k <= rep.n && !rep.abort; k++) {
      rep.sample = k;
      do_homing_move(ax, -5, fr_down, false);
      do_homing_move(ax, 10, fr_up, true);          // starts from 0, stops at the switch
      const float travel = stepper.triggered_position(ax) * planner.mm_per_step[ax];
      if (k) { at += travel - 5; rep.pos[t][k - 1] = at; }
    }
  }
  endstops.not_homing();
  gcode.process_subcommands_now(F("G28"));
  rep.running = false;
  rep.done = !rep.abort;
}

// ===========================================================================
// M2006: manual calibration moves
// ===========================================================================

static void cal_frame(const bool enter) {
  if (enter == cal_active) return;
  delta_height += enter ? qqs_settings.layer_offset : -qqs_settings.layer_offset;
  recalc_delta_settings();
  cal_active = enter;
  TERN_(HAS_SOFTWARE_ENDSTOPS, soft_endstop._loose = enter);   // the paper test may go below Z0 (down to -2)
}

// Travel at Z5 (coming down from the top after G28 at the centre first: the towers' points
// aren't reachable up there), then down to z.
static void go_point(const uint8_t i, const float z, const float approach_fr) {
  constexpr float travel = 5;
  if (current_position.z > travel + 0.01f) do_blocking_move_to_z(travel, 50);
  else if (current_position.z < travel)    do_blocking_move_to_z(travel, 10);
  do_blocking_move_to_xy(cal_pts[i][0], cal_pts[i][1], 80);
  do_blocking_move_to_z(z, approach_fr);
}

void qqs_M2006() {
  planner.synchronize();

  if (parser.seen('S')) {                       // enter / leave
    cal_frame(parser.value_bool());
    gcode.process_subcommands_now(F("G28"));
    return;
  }

  if (parser.seen_test('A')) {                  // apply the solved values (calibration frame)
    delta_height = cal_res.H;
    LOOP_ABC(i) delta_endstop_adj[i] = cal_res.adj[i];
    delta_radius = cal_res.R;
    recalc_delta_settings();
    gcode.process_subcommands_now(F("G28"));
    return;
  }

  if (!all_axes_homed()) gcode.process_subcommands_now(F("G28"));
  const float z = parser.seenval('Z') ? _MAX(-2.0f, parser.value_float()) : 1.5f;

  if (parser.seenval('R')) {                    // re-home and come back to the same height
    const uint8_t i = _MIN(parser.value_byte(), CAL_MAX_PTS - 1);
    do_blocking_move_to_z(_MAX(current_position.z, 5.0f), 10);
    gcode.process_subcommands_now(F("G28"));
    go_point(i, z + 1, 10);
    do_blocking_move_to_z(z, 1);
    return;
  }

  if (parser.seenval('P')) {                    // go to a point
    go_point(_MIN(parser.value_byte(), CAL_MAX_PTS - 1), z, 10);
    return;
  }

  // Plain Z adjust
  if (z > current_position.z && qqs_settings.one_way_approach) do_blocking_move_to_z(z + 0.2f, 5);
  do_blocking_move_to_z(z, 5);
}

// ===========================================================================
// M2007: v3.3 probe tools
// ===========================================================================

namespace qqs { ProbeJob pj; }
float qqs_g33_sd = NAN;   // set by G33 when it reports "Calibration OK"
bool qqs_probe_hold = false;   // M2007 A: probe stays "deployed" (probe.cpp skips deploy / stow)

static void pj_begin(const char kind) {
  pj.kind = kind; pj.ok = false; pj.done = false; pj.running = true;
  pj.start_ms = millis(); pj.end_ms = 0;
}
static void pj_end(const bool ok) {
  pj.ok = ok; pj.running = false; pj.done = true; pj.end_ms = millis();
}

// G33 in the calibration frame, old / new values into pj. Returns whether G33 got a result.
static bool do_g33(const uint8_t pts) {
  pj.oldH = delta_height; pj.oldR = delta_radius;
  LOOP_ABC(i) { pj.old_adj[i] = delta_endstop_adj[i]; pj.old_ang[i] = delta_tower_angle_trim[i]; }
  pj.sd[0] = 0;
  pj.had_mesh = bedlevel.has_mesh();
  cal_frame(true);
  char cmd[16];
  sprintf_P(cmd, PSTR("G33 P%u"), constrain(pts, 2, 10));
  qqs_g33_sd = NAN;
  gcode.process_subcommands_now(cmd);
  const bool ok = !isnan(qqs_g33_sd);          // G33 got to "Calibration OK" (probe failures return before)
  if (ok) { const uint32_t u = uint32_t(constrain(lroundf(qqs_g33_sd * 1000.0f), 0L, 99999L)); sprintf_P(pj.sd, PSTR("%u.%03u"), unsigned(u / 1000) % 100, unsigned(u % 1000)); }
  cal_frame(false);                             // put the layer offset back into the new H
  pj.newH = delta_height; pj.newR = delta_radius;
  LOOP_ABC(i) { pj.new_adj[i] = delta_endstop_adj[i]; pj.new_ang[i] = delta_tower_angle_trim[i]; }
  return ok;
}

// G28 + G29 in the calibration frame. Returns whether a mesh is there and on.
static bool do_g29() {
  cal_frame(true);
  gcode.process_subcommands_now(F("G28\nG29"));
  cal_frame(false);
  return bedlevel.has_mesh() && planner.leveling_active;
}

// Wait (UI running) for the probe to go untriggered -> triggered (20 ms stable). false = cancelled.
static bool wait_probe_tap() {
  KEEPALIVE_STATE(PAUSED_FOR_USER);
  bool was_free = false;
  millis_t on_ms = 0;
  while (!pj.abort) {
    idle();
    if (!PROBE_TRIGGERED()) { was_free = true; on_ms = 0; continue; }
    if (!was_free) continue;
    if (!on_ms) on_ms = millis();
    else if (ELAPSED(millis(), on_ms + 20)) return true;
  }
  return false;
}

// Short quick move up after the tap: Z+5 at 3000 mm/s^2 (limits restored afterwards)
static void tap_lift() {
  const float ta = planner.settings.travel_acceleration;
  uint32_t mx[3];
  LOOP_ABC(i) { mx[i] = planner.settings.max_acceleration_mm_per_s2[i]; NOLESS(planner.settings.max_acceleration_mm_per_s2[i], 3000UL); }
  planner.settings.travel_acceleration = 3000;
  planner.refresh_acceleration_rates();
  do_blocking_move_to_z(current_position.z + 5, 50);
  planner.settings.travel_acceleration = ta;
  LOOP_ABC(i) planner.settings.max_acceleration_mm_per_s2[i] = mx[i];
  planner.refresh_acceleration_rates();
}

void qqs_M2007() {
  planner.synchronize();

  if (parser.seen('R')) {                       // repeatability at the centre
    pj_begin('R');
    pj.n = constrain(parser.value_byte() ?: 10, 2, PRB_MAX);
    pj.i = 0;
    if (!all_axes_homed()) gcode.process_subcommands_now(F("G28"));
    bool ok = true;
    for (uint8_t k = 0; k < pj.n; k++) {
      const float z = probe.probe_at_point(0, 0, k + 1 < pj.n ? PROBE_PT_RAISE : PROBE_PT_STOW, 0, true, false);
      if (isnan(z)) { ok = false; probe.stow(); break; }
      pj.z[k] = z; pj.i = k + 1;
    }
    pj_end(ok);
    return;
  }

  if (parser.seen_test('H')) {                  // probe height, step 1 (H: 'P' is the G33 points of C / A)
    pj_begin('P');
    cal_frame(true);
    gcode.process_subcommands_now(F("G28"));
    pj.measured = probe.probe_at_point(0, 0, PROBE_PT_STOW, 0, true, false);
    const bool ok = !isnan(pj.measured);
    if (ok) {
      do_blocking_move_to_z(_MAX(current_position.z, 5.0f), 10);
      do_blocking_move_to_xy(0, 0, 80);
      do_blocking_move_to_z(1.0f, 5);          // paper test starts 1 mm above the centre
    }
    pj_end(ok);
    return;
  }

  if (parser.seen_test('C')) {                  // delta auto calibration
    pj_begin('C');
    const bool ok = do_g33(parser.byteval('P', 3));
    gcode.process_subcommands_now(F("G28"));
    pj_end(ok);
    return;
  }

  if (parser.seen_test('U')) {                  // undo the last calibration: G33 also cleared the mesh, so reload everything
    settings.load();
    gcode.process_subcommands_now(F("G28"));
    return;
  }

  if (parser.seen_test('M')) {                  // bilinear mesh
    pj_begin('M');
    const bool ok = do_g29();
    gcode.process_subcommands_now(F("G28"));     // leveling stays on (RESTORE_LEVELING_AFTER_G28)
    pj_end(ok);
    return;
  }

  if (parser.seen_test('A')) {                  // one go: tap the probe, G33, G29
    pj_begin('A');
    pj.abort = false; pj.g33_ok = pj.mesh_ok = false;
    const uint8_t pts = parser.byteval('P', 3);
    pj.stage = ProbeJob::AS_HOME;
    gcode.process_subcommands_now(F("G28"));
    do_blocking_move_to_xy(0, 0, 80);
    do_blocking_move_to_z(current_position.z - 20, 20);
    pj.stage = ProbeJob::AS_WAIT;
    bool go = wait_probe_tap();
    if (go) {
      tap_lift();
      TERN_(HAS_BUZZER, BUZZ(50, 2400));
      pj.stage = ProbeJob::AS_COUNT;
      for (uint8_t c = 3; c && !pj.abort; c--) {
        pj.count = c;
        const millis_t end = millis() + 1000;
        while (PENDING(millis(), end) && !pj.abort) idle();
      }
      go = !pj.abort;
    }
    if (go) {
      qqs_probe_hold = true;                    // the probe is on: no deploy / stow prompts until the end
      endstops.enable_z_probe(true);
      pj.stage = ProbeJob::AS_G33;
      gcode.process_subcommands_now(F("G28"));
      pj.g33_ok = do_g33(pts);
      if (pj.g33_ok) {                          // G33 ends homed (at the top): G29 starts from there
        pj.stage = ProbeJob::AS_G29;
        pj.mesh_ok = do_g29();
      }
      qqs_probe_hold = false;
      endstops.enable_z_probe(true);            // so stow() below really asks to take it off
      pj.stage = ProbeJob::AS_STOW;
      probe.stow();
    }
    gcode.process_subcommands_now(F("G28"));
    pj.stage = ProbeJob::AS_DONE;
    pj_end(go && pj.g33_ok && pj.mesh_ok);
    return;
  }
}

#endif // QQS_UI
