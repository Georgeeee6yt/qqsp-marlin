# Smooth LA (v3.2.9) tools

- `sla_sim_v54_b1.py`: George's B1 host simulation (V5.4), unchanged copy.
- `sla_sim_qqs.py`: same + S-curve (Bezier) in the stepper and the lookahead, as the firmware does it; `E_VMAX` (M900 V).
- `run_qqs.py`: QQS-Pro numbers (E 415, accel 1000, 0.4 x 0.2). Output of 2026-11-25 in `run_qqs_output.txt`.
- `make_la_tests.py [outdir]`: LA0 air extrusion test, LA1 K / LA2 U / LA3 J towers for the round bed.

Firmware side: `Marlin/src/module/stepper.cpp` (search "[George]" / "[QQS-Pro]"), planner.cpp/h, M900.cpp,
Configuration_adv.h (LIN_ADVANCE block), performance counters `Marlin/src/feature/qqs_perf.cpp` (M2011).
