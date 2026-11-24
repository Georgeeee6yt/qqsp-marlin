/**
 * QQS-Pro v3 UI — settings kept in EEPROM (saved with M500, part of SettingsData).
 */
#pragma once

#include <stdint.h>

typedef struct __attribute__((__packed__)) {
  float   layer_offset;        // (mm) first-layer offset already folded into M665 H; + = nozzle raised
  uint8_t test_count;          // endstop repeatability test: samples per tower (5 or 10)
  uint8_t soak_minutes;        // calibration: wait after reaching temperature (0-5)
  uint8_t one_way_approach;    // calibration: always finish a Z adjustment moving down
  uint8_t show_remeasure_diff; // calibration: show the difference after "re-home and measure again"
  uint8_t probe_installed;     // sensor page: show the probe as installed (0 = dashed "not installed")
  uint8_t anim_off;            // printing screen: still picture instead of the animation
  uint8_t reserved[6];
} qqs_settings_t;

extern qqs_settings_t qqs_settings;

void qqs_settings_reset();
