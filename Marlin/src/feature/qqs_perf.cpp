/**
 * QQS-Pro v3.2.9: performance counters, see qqs_perf.h
 */
#include "../inc/MarlinConfig.h"

#if ENABLED(QQS_PERF)

#include "qqs_perf.h"
#include "../MarlinCore.h"
#include "../module/planner.h"

namespace qqs_perf {

  Acc isr, sla, plan, ik;
  volatile uint32_t blocks, low_blocks, empty_events;
  volatile bool printing, had_block;
  static millis_t since_ms;
  static bool printed_in_window;

  void init() {
    volatile uint32_t * const DEMCR    = (volatile uint32_t *)0xE000EDFCUL,
                      * const DWT_CTRL = (volatile uint32_t *)0xE0001000UL,
                      * const CYCCNT   = (volatile uint32_t *)0xE0001004UL;
    *DEMCR |= 1UL << 24;          // TRCENA
    *CYCCNT = 0;
    *DWT_CTRL |= 1UL;             // CYCCNTENA
    since_ms = millis();
  }

  void poll() {
    printing = printingIsActive();
    if (printing) printed_in_window = true;
  }

  static void reset() {
    hal.isr_off();
    isr = sla = plan = ik = Acc();
    blocks = low_blocks = empty_events = 0;
    hal.isr_on();
    since_ms = millis();
    printed_in_window = printing;
  }

  // "12.3" from a value x 10
  static void echo_d1(const uint32_t v10) { SERIAL_ECHO(v10 / 10); SERIAL_CHAR('.'); SERIAL_ECHO(v10 % 10); }

  void report(const bool do_reset) {
    hal.isr_off();
    const Acc i = isr, s = sla;
    const uint32_t b = blocks, lb = low_blocks, ee = empty_events;
    hal.isr_on();
    const Acc p = plan, k = ik;

    const uint32_t ms = _MAX(millis() - since_ms, 1UL);
    const uint64_t window = uint64_t(ms) * (F_CPU / 1000UL);        // cycles in the window
    constexpr uint32_t cpu_mhz = F_CPU / 1000000UL;
    auto pct10 = [&](const uint64_t c) { return uint32_t(c * 1000ULL / window); };   // % x 10
    auto us = [&](const uint32_t c) { return c / cpu_mhz; };
    auto avg_us = [&](const Acc &a) { return a.n ? uint32_t(a.cyc / a.n / cpu_mhz) : 0UL; };

    SERIAL_ECHOPGM("QQS perf "); echo_d1(ms / 100); SERIAL_ECHOPGM(" s");
    SERIAL_ECHOLNF(printed_in_window ? F(" (printing)") : F(" (idle)"));

    SERIAL_ECHOPGM(" stepper ISR: "); echo_d1(pct10(i.cyc));
    SERIAL_ECHOLNPGM("% CPU, max ", us(i.max), " us, ", uint32_t(uint64_t(i.n) * 1000 / ms), " calls/s");
    #if ENABLED(SMOOTH_LIN_ADVANCE)
      SERIAL_ECHOPGM("   smooth LA update: "); echo_d1(pct10(s.cyc));
      SERIAL_ECHOLNPGM("% CPU, avg ", avg_us(s), " us, max ", us(s.max), " us");
    #else
      UNUSED(s);
    #endif
    SERIAL_ECHOPGM(" planner: ", p.n, " blocks (", uint32_t(uint64_t(p.n) * 1000 / ms), "/s), avg ", avg_us(p), " us, max ", us(p.max), " us = ");
    echo_d1(pct10(p.cyc)); SERIAL_ECHOLNPGM("% CPU");
    #if ENABLED(DELTA)
      SERIAL_ECHOPGM(" delta IK: ", k.n, " calls, avg ", avg_us(k), " us = "); echo_d1(pct10(k.cyc)); SERIAL_ECHOLNPGM("% CPU");
    #else
      UNUSED(k);
    #endif
    SERIAL_ECHOLNPGM(" buffer (printing): ", b, " blocks, ", lb, " started with <3 planned, ran empty ", ee, " times");

    if (do_reset) { reset(); SERIAL_ECHOLNPGM(" (reset)"); }
  }

} // qqs_perf

#endif // QQS_PERF
