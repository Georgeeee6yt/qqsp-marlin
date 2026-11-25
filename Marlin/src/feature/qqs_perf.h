/**
 * QQS-Pro v3.2.9: performance counters (DWT cycle counter, 72 MHz on the F103).
 *
 *   M2011     report since the last reset      M2011 R   reset the counters
 *
 * Stepper ISR: share of the CPU, longest single run, calls per second; the Smooth LA rate update
 * separately. Planner: cycles of _populate_block() + recalculate() per queued block (not the time
 * spent waiting for a free block). Delta IK: inverse_kinematics(). Buffer: while a print runs,
 * blocks that started with fewer than 3 blocks planned, and how often the planner ran empty.
 * All counters are 32/64-bit adds; the ISR part costs a few cycles per call.
 */
#pragma once

#include "../inc/MarlinConfigPre.h"

#if ENABLED(QQS_PERF)

#include <stdint.h>

namespace qqs_perf {

  struct Acc {
    uint64_t cyc;     // total cycles
    uint32_t n,       // calls
             max;     // longest single call (cycles)
    inline void add(const uint32_t c) { cyc += c; n++; if (c > max) max = c; }
  };

  extern Acc isr, sla, plan, ik;
  extern volatile uint32_t blocks, low_blocks, empty_events;
  extern volatile bool printing, had_block;

  // DWT->CYCCNT, without pulling the CMSIS headers into every user
  inline uint32_t now() { return *(volatile uint32_t *)0xE0001004UL; }

  void init();                    // enable the cycle counter (setup)
  void poll();                    // main loop: keep the "printing" flag current
  void report(const bool reset);  // M2011

  // Stepper ISR side
  inline void block_start(const uint8_t planned) {
    had_block = true;
    if (printing) { blocks++; if (planned < 3) low_blocks++; }
  }
  inline void block_none() {
    if (had_block) { had_block = false; if (printing) empty_events++; }
  }

} // qqs_perf

#define QQS_PERF_START(V)   const uint32_t V = qqs_perf::now()
#define QQS_PERF_END(A, V)  qqs_perf::A.add(qqs_perf::now() - (V))

#else

#define QQS_PERF_START(V)   NOOP
#define QQS_PERF_END(A, V)  NOOP

#endif
