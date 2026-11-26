/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

/**
 * delta.cpp
 */

#include "../inc/MarlinConfig.h"

#if ENABLED(DELTA)

#include "delta.h"
#include "../feature/qqs_perf.h"
#include "motion.h"

// For homing:
#include "planner.h"
#include "endstops.h"
#include "../lcd/marlinui.h"
#include "../MarlinCore.h"

#if HAS_BED_PROBE
  #include "probe.h"
#endif

#if ENABLED(SENSORLESS_HOMING)
  #include "../feature/tmc_util.h"
  #include "stepper/indirection.h"
#endif

#define DEBUG_OUT ENABLED(DEBUG_LEVELING_FEATURE)
#include "../core/debug_out.h"

// Initialized by settings.load()
float delta_height;
abc_float_t delta_endstop_adj{0};
float delta_radius,
      delta_diagonal_rod,
      segments_per_second;
abc_float_t delta_tower_angle_trim;
xy_float_t delta_tower[ABC];
abc_float_t delta_diagonal_rod_2_tower;
float delta_clip_start_height = Z_MAX_POS;
abc_float_t delta_diagonal_rod_trim;

float delta_safe_distance_from_top();

void refresh_delta_clip_start_height() {
  delta_clip_start_height = TERN(HAS_SOFTWARE_ENDSTOPS,
    soft_endstop.max.z,
    DIFF_TERN(HAS_BED_PROBE, delta_height, probe.offset.z)
  ) - delta_safe_distance_from_top();
}

/**
 * Recalculate factors used for delta kinematics whenever
 * settings have been changed (e.g., by M665).
 */
void recalc_delta_settings() {
  constexpr abc_float_t trt = DELTA_RADIUS_TRIM_TOWER;
  delta_tower[A_AXIS].set(cos(RADIANS(210 + delta_tower_angle_trim.a)) * (delta_radius + trt.a), // front left tower
                          sin(RADIANS(210 + delta_tower_angle_trim.a)) * (delta_radius + trt.a));
  delta_tower[B_AXIS].set(cos(RADIANS(330 + delta_tower_angle_trim.b)) * (delta_radius + trt.b), // front right tower
                          sin(RADIANS(330 + delta_tower_angle_trim.b)) * (delta_radius + trt.b));
  delta_tower[C_AXIS].set(cos(RADIANS( 90 + delta_tower_angle_trim.c)) * (delta_radius + trt.c), // back middle tower
                          sin(RADIANS( 90 + delta_tower_angle_trim.c)) * (delta_radius + trt.c));
  delta_diagonal_rod_2_tower.set(sq(delta_diagonal_rod + delta_diagonal_rod_trim.a),
                                 sq(delta_diagonal_rod + delta_diagonal_rod_trim.b),
                                 sq(delta_diagonal_rod + delta_diagonal_rod_trim.c));
  update_software_endstops(Z_AXIS);
  set_all_unhomed();
}

/**
 * Delta Inverse Kinematics
 *
 * Calculate the tower positions for a given machine
 * position, storing the result in the delta[] array.
 *
 * This is an expensive calculation, requiring 3 square
 * roots per segmented linear move, and strains the limits
 * of a Mega2560 with a Graphical Display.
 *
 * Suggested optimizations include:
 *
 * - Disable the home_offset (M206) and/or position_shift (G92)
 *   features to remove up to 12 float additions.
 */

#define DELTA_DEBUG(VAR) do { \
    SERIAL_ECHOLNPGM_P(PSTR("Cartesian X"), VAR.x, SP_Y_STR, VAR.y, SP_Z_STR, VAR.z); \
    SERIAL_ECHOLNPGM_P(PSTR("Delta A"), delta.a, SP_B_STR, delta.b, SP_C_STR, delta.c); \
  }while(0)

void inverse_kinematics(const xyz_pos_t &raw) {
  QQS_PERF_START(perf_k0);
  #if HAS_HOTEND_OFFSET
    // Delta hotend offsets must be applied in Cartesian space with no "spoofing"
    xyz_pos_t pos = { raw.x - hotend_offset[active_extruder].x,
                      raw.y - hotend_offset[active_extruder].y,
                      raw.z };
    DELTA_IK(pos);
    //DELTA_DEBUG(pos);
  #else
    DELTA_IK(raw);
    //DELTA_DEBUG(raw);
  #endif
  QQS_PERF_END(ik, perf_k0);
}

/**
 * Calculate the highest Z position where the
 * effector has the full range of XY motion.
 */
float delta_safe_distance_from_top() {
  xyz_pos_t cartesian{0};
  inverse_kinematics(cartesian);
  const float centered_extent = delta.a;
  cartesian.y = DELTA_PRINTABLE_RADIUS;
  inverse_kinematics(cartesian);
  return ABS(centered_extent - delta.a);
}

/**
 * Delta Forward Kinematics
 *
 * See the Wikipedia article "Trilateration"
 * https://en.wikipedia.org/wiki/Trilateration
 *
 * Establish a new coordinate system in the plane of the
 * three carriage points. This system has its origin at
 * tower1, with tower2 on the X axis. Tower3 is in the X-Y
 * plane with a Z component of zero.
 * We will define unit vectors in this coordinate system
 * in our original coordinate system. Then when we calculate
 * the Xnew, Ynew and Znew values, we can translate back into
 * the original system by moving along those unit vectors
 * by the corresponding values.
 *
 * Variable names matched to Marlin, c-version, and avoid the
 * use of any vector library.
 *
 * by Andreas Hardtung 2016-06-07
 * based on a Java function from "Delta Robot Kinematics V3"
 * by Steve Graves
 *
 * The result is stored in the cartes[] array.
 */
void forward_kinematics(const_float_t z1, const_float_t z2, const_float_t z3) {
  // Create a vector in old coordinates along x axis of new coordinate
  const float p12[3] = { delta_tower[B_AXIS].x - delta_tower[A_AXIS].x, delta_tower[B_AXIS].y - delta_tower[A_AXIS].y, z2 - z1 },

  // Get the reciprocal of Magnitude of vector.
  d2 = sq(p12[0]) + sq(p12[1]) + sq(p12[2]), inv_d = RSQRT(d2),

  // Create unit vector by multiplying by the inverse of the magnitude.
  ex[3] = { p12[0] * inv_d, p12[1] * inv_d, p12[2] * inv_d },

  // Get the vector from the origin of the new system to the third point.
  p13[3] = { delta_tower[C_AXIS].x - delta_tower[A_AXIS].x, delta_tower[C_AXIS].y - delta_tower[A_AXIS].y, z3 - z1 },

  // Use the dot product to find the component of this vector on the X axis.
  i = ex[0] * p13[0] + ex[1] * p13[1] + ex[2] * p13[2],

  // Create a vector along the x axis that represents the x component of p13.
  iex[3] = { ex[0] * i, ex[1] * i, ex[2] * i };

  // Subtract the X component from the original vector leaving only Y. We use the
  // variable that will be the unit vector after we scale it.
  float ey[3] = { p13[0] - iex[0], p13[1] - iex[1], p13[2] - iex[2] };

  // The magnitude and the inverse of the magnitude of Y component
  const float j2 = sq(ey[0]) + sq(ey[1]) + sq(ey[2]), inv_j = RSQRT(j2);

  // Convert to a unit vector
  ey[0] *= inv_j; ey[1] *= inv_j; ey[2] *= inv_j;

  // The cross product of the unit x and y is the unit z
  // float[] ez = vectorCrossProd(ex, ey);
  const float ez[3] = {
    ex[1] * ey[2] - ex[2] * ey[1],
    ex[2] * ey[0] - ex[0] * ey[2],
    ex[0] * ey[1] - ex[1] * ey[0]
  },

  // We now have the d, i and j values defined in Wikipedia.
  // Plug them into the equations defined in Wikipedia for Xnew, Ynew and Znew
  Xnew = (delta_diagonal_rod_2_tower.a - delta_diagonal_rod_2_tower.b + d2) * inv_d * 0.5,
  Ynew = ((delta_diagonal_rod_2_tower.a - delta_diagonal_rod_2_tower.c + sq(i) + j2) * 0.5 - i * Xnew) * inv_j,
  Znew = SQRT(delta_diagonal_rod_2_tower.a - HYPOT2(Xnew, Ynew));

  // Start from the origin of the old coordinates and add vectors in the
  // old coords that represent the Xnew, Ynew and Znew to find the point
  // in the old system.
  cartes.set(delta_tower[A_AXIS].x + ex[0] * Xnew + ey[0] * Ynew - ez[0] * Znew,
             delta_tower[A_AXIS].y + ex[1] * Xnew + ey[1] * Ynew - ez[1] * Znew,
                                z1 + ex[2] * Xnew + ey[2] * Ynew - ez[2] * Znew);
}

/**
 * A delta can only safely home all axes at the same time
 * This is like quick_home_xy() but for 3 towers.
 */
#if ENABLED(QQS_UI)

// QQS-Pro: tower homing like the stock FLSUN firmware. Marlin stops every tower at the first switch (an endstop
// aborts the whole block) and then re-homes the towers one at a time, so the effector swings from side to side.
// Here the towers still short of their switch carry on together until they are all there, once fast, then
// (after backing off all three together) once slowly.

static bool qqs_tower_at_switch(const uint8_t i) {
  switch (i) {
    case 0:  return READ(X_MAX_PIN) != X_MAX_ENDSTOP_INVERTING;
    case 1:  return READ(Y_MAX_PIN) != Y_MAX_ENDSTOP_INVERTING;
    default: return READ(Z_MAX_PIN) != Z_MAX_ENDSTOP_INVERTING;
  }
}

// Move the carriages by d (mm, + = up) in one block; an endstop of a moving tower stops all of them.
void qqs_carriage_move(const float d[3], const feedRate_t fr_mm_s) {
  abce_pos_t t = planner.get_axis_positions_mm();   // where the steppers really are (also after an abort)
  planner.set_machine_position_mm(t);
  LOOP_ABC(i) t[i] += d[i];
  #if HAS_DIST_MM_ARG
    const xyze_float_t cart_dist_mm{0};
  #endif
  planner.buffer_segment(t OPTARG(HAS_DIST_MM_ARG, cart_dist_mm), fr_mm_s, active_extruder);
  planner.synchronize();
}

// Every tower up to its own switch, the ones still free moving together. False if one never got there.
static bool qqs_towers_to_switches(const float dist, const feedRate_t fr_mm_s) {
  LOOP_L_N(pass, 3) {
    float d[3];
    bool any = false;
    LOOP_ABC(i) { d[i] = qqs_tower_at_switch(i) ? 0 : dist; any |= d[i] != 0; }
    if (!any) break;
    qqs_carriage_move(d, fr_mm_s);
    endstops.hit_on_purpose();
  }
  LOOP_ABC(i) if (!qqs_tower_at_switch(i)) return false;
  return true;
}

static void qqs_home_towers() {
  const float far = 1.5f * (delta_height + 10);
  if (!qqs_towers_to_switches(far, homing_feedrate(Z_AXIS))) { kill(GET_TEXT_F(MSG_KILL_HOMING_FAILED)); return; }
  constexpr xyz_float_t bump = HOMING_BUMP_MM;
  const feedRate_t slow = get_homing_bump_feedrate(Z_AXIS);
  const float back[3] = { -bump.z, -bump.z, -bump.z };
  qqs_carriage_move(back, homing_feedrate(Z_AXIS));
  if (!qqs_towers_to_switches(bump.z * 2, slow)) { kill(GET_TEXT_F(MSG_KILL_HOMING_FAILED)); return; }
  // M666: each tower ends that far below its switch (Marlin does it per tower in homeaxis)
  float adj[3];
  bool any = false;
  LOOP_ABC(i) {
    const float a = delta_endstop_adj[i], mind = (MIN_STEPS_PER_SEGMENT) * planner.mm_per_step[i];
    adj[i] = (a * (Z_HOME_DIR) < 0 && ABS(a) > mind) ? a : 0;
    any |= adj[i] != 0;
  }
  if (any) qqs_carriage_move(adj, slow);
}

#endif // QQS_UI

void home_delta() {
  DEBUG_SECTION(log_home_delta, "home_delta", DEBUGGING(LEVELING));

  // Init the current position of all carriages to 0,0,0
  current_position.reset();
  destination.reset();
  sync_plan_position();

  // Disable stealthChop if used. Enable diag1 pin on driver.
  #if ENABLED(SENSORLESS_HOMING)
    TERN_(X_SENSORLESS, sensorless_t stealth_states_x = start_sensorless_homing_per_axis(X_AXIS));
    TERN_(Y_SENSORLESS, sensorless_t stealth_states_y = start_sensorless_homing_per_axis(Y_AXIS));
    TERN_(Z_SENSORLESS, sensorless_t stealth_states_z = start_sensorless_homing_per_axis(Z_AXIS));
    TERN_(I_SENSORLESS, sensorless_t stealth_states_i = start_sensorless_homing_per_axis(I_AXIS));
    TERN_(J_SENSORLESS, sensorless_t stealth_states_j = start_sensorless_homing_per_axis(J_AXIS));
    TERN_(K_SENSORLESS, sensorless_t stealth_states_k = start_sensorless_homing_per_axis(K_AXIS));
    TERN_(U_SENSORLESS, sensorless_t stealth_states_u = start_sensorless_homing_per_axis(U_AXIS));
    TERN_(V_SENSORLESS, sensorless_t stealth_states_v = start_sensorless_homing_per_axis(V_AXIS));
    TERN_(W_SENSORLESS, sensorless_t stealth_states_w = start_sensorless_homing_per_axis(W_AXIS));
    #if SENSORLESS_STALLGUARD_DELAY
      safe_delay(SENSORLESS_STALLGUARD_DELAY); // Short delay needed to settle
    #endif
  #endif

  #if ENABLED(QQS_UI)
    qqs_home_towers();
  #else
  // Move all carriages together linearly until an endstop is hit.
  current_position.z = DIFF_TERN(HAS_BED_PROBE, delta_height + 10, probe.offset.z);
  line_to_current_position(homing_feedrate(Z_AXIS));
  planner.synchronize();
  TERN_(HAS_DELTA_SENSORLESS_PROBING, endstops.report_states());
  #endif

  // Re-enable stealthChop if used. Disable diag1 pin on driver.
  #if ENABLED(SENSORLESS_HOMING) && DISABLED(ENDSTOPS_ALWAYS_ON_DEFAULT)
    TERN_(X_SENSORLESS, end_sensorless_homing_per_axis(X_AXIS, stealth_states_x));
    TERN_(Y_SENSORLESS, end_sensorless_homing_per_axis(Y_AXIS, stealth_states_y));
    TERN_(Z_SENSORLESS, end_sensorless_homing_per_axis(Z_AXIS, stealth_states_z));
    TERN_(I_SENSORLESS, end_sensorless_homing_per_axis(I_AXIS, stealth_states_i));
    TERN_(J_SENSORLESS, end_sensorless_homing_per_axis(J_AXIS, stealth_states_j));
    TERN_(K_SENSORLESS, end_sensorless_homing_per_axis(K_AXIS, stealth_states_k));
    TERN_(U_SENSORLESS, end_sensorless_homing_per_axis(U_AXIS, stealth_states_u));
    TERN_(V_SENSORLESS, end_sensorless_homing_per_axis(V_AXIS, stealth_states_v));
    TERN_(W_SENSORLESS, end_sensorless_homing_per_axis(W_AXIS, stealth_states_w));
    #if SENSORLESS_STALLGUARD_DELAY
      safe_delay(SENSORLESS_STALLGUARD_DELAY); // Short delay needed to settle
    #endif
  #endif

  #if DISABLED(QQS_UI)
  endstops.validate_homing_move();

  // At least one carriage has reached the top.
  // Now re-home each carriage separately.
  homeaxis(A_AXIS);
  homeaxis(B_AXIS);
  homeaxis(C_AXIS);
  #endif

  // Set all carriages to their home positions
  // Do this here all at once for Delta, because
  // XYZ isn't ABC. Applying this per-tower would
  // give the impression that they are the same.
  LOOP_ABC(i) set_axis_is_at_home((AxisEnum)i);

  sync_plan_position();

  #if DISABLED(DELTA_HOME_TO_SAFE_ZONE) && defined(HOMING_BACKOFF_POST_MM)
    constexpr xyz_float_t endstop_backoff = HOMING_BACKOFF_POST_MM;
    if (endstop_backoff.z) {
      current_position.z -= ABS(endstop_backoff.z) * Z_HOME_DIR;
      line_to_current_position(homing_feedrate(Z_AXIS));
    }
  #endif
}

#endif // DELTA
