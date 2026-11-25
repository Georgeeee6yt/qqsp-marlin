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

#include "../../../inc/MarlinConfig.h"

#if ENABLED(LIN_ADVANCE)

#include "../../gcode.h"
#include "../../../module/planner.h"
#include "../../../module/stepper.h"

#if ENABLED(ADVANCE_K_EXTRA)
  float other_extruder_advance_K[DISTINCT_E];
  uint8_t lin_adv_slot = 0;
#endif

/**
 * M900: Get or Set Linear Advance K-factor
 *  T<tool>     Which tool to address
 *  K<factor>   Set current advance K factor (Slot 0).
 *  L<factor>   Set secondary advance K factor (Slot 1). Requires ADVANCE_K_EXTRA.
 *  S<0/1>      Activate slot 0 or 1. Requires ADVANCE_K_EXTRA.
 */
void GcodeSuite::M900() {

  auto echo_value_oor = [](const char ltr, const bool ten=true) {
    SERIAL_CHAR('?', ltr);
    SERIAL_ECHOPGM(" value out of range");
    if (ten) SERIAL_ECHOPGM(" (0-10)");
    SERIAL_ECHOLNPGM(".");
  };

  #if EXTRUDERS < 2
    constexpr uint8_t tool_index = 0;
    UNUSED(tool_index);
  #else
    const uint8_t tool_index = parser.intval('T', active_extruder);
    if (tool_index >= EXTRUDERS) {
      echo_value_oor('T', false);
      return;
    }
  #endif

  float &kref = planner.extruder_advance_K[E_INDEX_N(tool_index)], newK = kref;
  const float oldK = newK;

  #if ENABLED(ADVANCE_K_EXTRA)

    float &lref = other_extruder_advance_K[E_INDEX_N(tool_index)];

    const bool old_slot = TEST(lin_adv_slot, tool_index), // The tool's current slot (0 or 1)
               new_slot = parser.boolval('S', old_slot);  // The passed slot (default = current)

    // If a new slot is being selected swap the current and
    // saved K values. Do here so K/L will apply correctly.
    if (new_slot != old_slot) {                       // Not the same slot?
      SET_BIT_TO(lin_adv_slot, tool_index, new_slot); // Update the slot for the tool
      newK = lref;                                    // Get new K value from backup
      lref = oldK;                                    // Save K to backup
    }

    // Set the main K value. Apply if the main slot is active.
    if (parser.seenval('K')) {
      const float K = parser.value_float();
      if (!WITHIN(K, 0, 10)) echo_value_oor('K');
      else if (new_slot)        lref = K;             // S1 Knn
      else                      newK = K;             // S0 Knn
    }

    // Set the extra K value. Apply if the extra slot is active.
    if (parser.seenval('L')) {
      const float L = parser.value_float();
      if (!WITHIN(L, 0, 10)) echo_value_oor('L');
      else if (!new_slot)       lref = L;             // S0 Lnn
      else                      newK = L;             // S1 Lnn
    }

  #else

    if (parser.seenval('K')) {
      const float K = parser.value_float();
      if (WITHIN(K, 0, 10))
        newK = K;
      else
        echo_value_oor('K');
    }

  #endif

  // [George] V5: U<seconds> = Smooth LA tau (smoothing / lookahead time). Not saved by M500.
  #if ENABLED(SMOOTH_LIN_ADVANCE)
    const float oldU = stepper.get_advance_tau();
    float newU = oldU;
    if (parser.seenval('U')) {
      const float U = parser.value_float();
      if (WITHIN(U, 0.0f, 0.5f)) newU = U;
      else { SERIAL_ECHOLNPGM("?U value out of range (0-0.5)."); }
    }
  #endif

  // [George] V5.2: J<release K> (0 = same as K), B<build tau>, R<ms>. Not saved by M500.
  #if ENABLED(SMOOTH_LA_RELEASE_GAIN)
    const float oldJ = stepper.get_release_k(), oldB = stepper.get_build_tau(),
                oldR = stepper.get_refill_mms(), oldA = stepper.get_e_accel(), oldV = stepper.get_e_vmax();
    float newJ = oldJ, newB = oldB, newR = oldR, newA = oldA, newV = oldV;
    if (parser.seenval('J')) {
      const float J = parser.value_float();
      if (WITHIN(J, 0.0f, 10.0f)) newJ = J;
      else { SERIAL_ECHOLNPGM("?J value out of range (0-10)."); }
    }
    if (parser.seenval('B')) {
      const float B = parser.value_float();
      if (WITHIN(B, 0.0f, 0.5f)) newB = B;
      else { SERIAL_ECHOLNPGM("?B value out of range (0-0.5)."); }
    }
    if (parser.seenval('R')) {
      const float R = parser.value_float();
      if (WITHIN(R, 1.0f, 60.0f)) newR = R;
      else { SERIAL_ECHOLNPGM("?R value out of range (1-60 mm/s)."); }
    }
    if (parser.seenval('A')) {
      const float A = parser.value_float();
      if (WITHIN(A, 100.0f, 20000.0f)) newA = A;
      else { SERIAL_ECHOLNPGM("?A value out of range (100-20000)."); }
    }
    if (parser.seenval('V')) {   // [QQS-Pro]
      const float V = parser.value_float();
      if (V == 0.0f || WITHIN(V, 5.0f, 100.0f)) newV = V;
      else { SERIAL_ECHOLNPGM("?V value out of range (0 or 5-100 mm/s)."); }
    }
  #endif

  if (newK != oldK || TERN0(SMOOTH_LIN_ADVANCE, newU != oldU) || TERN0(SMOOTH_LA_RELEASE_GAIN, newJ != oldJ || newB != oldB || newR != oldR || newA != oldA || newV != oldV)) {
    planner.synchronize();
    kref = newK;
    TERN_(SMOOTH_LIN_ADVANCE, if (newU != oldU) stepper.set_advance_tau(newU));
    #if ENABLED(SMOOTH_LA_RELEASE_GAIN)
      if (newJ != oldJ || newR != oldR) stepper.set_release(newJ, newR);
      if (newB != oldB) stepper.set_build_tau(newB);
      if (newA != oldA) stepper.set_e_accel(newA);
      if (newV != oldV) stepper.set_e_vmax(newV);
    #endif
  }

  if (!parser.seen_any()) {

    #if ENABLED(ADVANCE_K_EXTRA)

      #if DISTINCT_E < 2
        SERIAL_ECHOLNPGM("Advance S", new_slot, " K", kref, "(S", !new_slot, " K", lref, ")");
      #else
        EXTRUDER_LOOP() {
          const bool slot = TEST(lin_adv_slot, e);
          SERIAL_ECHOLNPGM("Advance T", e, " S", slot, " K", planner.extruder_advance_K[e],
                            "(S", !slot, " K", other_extruder_advance_K[e], ")");
          SERIAL_EOL();
        }
      #endif

    #else

      SERIAL_ECHO_START();
      #if DISTINCT_E < 2
        #if ENABLED(SMOOTH_LA_RELEASE_GAIN)
          SERIAL_ECHOLNPGM("Advance K=", planner.extruder_advance_K[0], " J=", stepper.get_release_k(),
                           " B=", stepper.get_build_tau(), " U=", stepper.get_advance_tau(), " R=", stepper.get_refill_mms(), " A=", stepper.get_e_accel(), " V=", stepper.get_e_vmax());
        #elif ENABLED(SMOOTH_LIN_ADVANCE)
          SERIAL_ECHOLNPGM("Advance K=", planner.extruder_advance_K[0], " TAU=", stepper.get_advance_tau());
        #else
          SERIAL_ECHOLNPGM("Advance K=", planner.extruder_advance_K[0]);
        #endif
      #else
        SERIAL_ECHOPGM("Advance K");
        EXTRUDER_LOOP() {
          SERIAL_CHAR(' ', '0' + e, ':');
          SERIAL_DECIMAL(planner.extruder_advance_K[e]);
        }
        SERIAL_EOL();
      #endif

    #endif
  }

}

void GcodeSuite::M900_report(const bool forReplay/*=true*/) {
  report_heading(forReplay, F(STR_LINEAR_ADVANCE));
  #if DISTINCT_E < 2
    report_echo_start(forReplay);
    #if ENABLED(SMOOTH_LA_RELEASE_GAIN)
      SERIAL_ECHOLNPGM("  M900 K", planner.extruder_advance_K[0], " J", stepper.get_release_k(),
                       " B", stepper.get_build_tau(), " U", stepper.get_advance_tau(), " R", stepper.get_refill_mms(), " A", stepper.get_e_accel(), " V", stepper.get_e_vmax());
    #elif ENABLED(SMOOTH_LIN_ADVANCE)
      SERIAL_ECHOLNPGM("  M900 K", planner.extruder_advance_K[0], " U", stepper.get_advance_tau());
    #else
      SERIAL_ECHOLNPGM("  M900 K", planner.extruder_advance_K[0]);
    #endif
  #else
    EXTRUDER_LOOP() {
      report_echo_start(forReplay);
      SERIAL_ECHOLNPGM("  M900 T", e, " K", planner.extruder_advance_K[e]);
    }
  #endif
}

#endif // LIN_ADVANCE
