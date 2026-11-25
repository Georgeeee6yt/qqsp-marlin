#!/usr/bin/env python3
"""QQS-Pro v3.2.9: run sla_sim_qqs.py (B1 V5.4 model + S-curve) with QQS-Pro numbers.
   E 415 steps/mm, towers ~100 steps/mm, accel 1000, 0.4 x 0.2 line on 1.75 filament."""
import importlib, math, sys
sys.argv = [sys.argv[0]]
sim = importlib.import_module('sla_sim_qqs')

def setup(K, J=0.0, B=0.01, U=0.02, R=15.0, A=5000.0, V=0.0, scurve=False, speed=60.0):
    sim.E_VMAX = V
    sim.K, sim.J, sim.TAU_B, sim.TAU_U, sim.TAU, sim.R_MMS, sim.E_ACC = K, J, B, U, U, R, A
    sim.POS = sim.V52 = sim.HOLD = sim.LIN = True
    sim.ESTEPS, sim.XYSTEPS, sim.ACCEL, sim.SPEED = 415.0, 100.0, 1000.0, speed
    sim.ED = 0.4 * 0.2 / (math.pi * 0.875 ** 2)
    sim.SCURVE = scurve

def paths(v):
    arc = [(math.cos(i * 0.1) * 0.5, math.sin(i * 0.1) * 0.5, v, 1) for i in range(120)]   # r = 5 mm circle-ish, 0.5 mm segments
    return [('square 40mm', sim.square(40, v)), ('line 150mm', [(150, 0, v, 1)]),
            ('slow line 5mm/s', [(20, 0, 5.0, 1)]), ('arc 120 x 0.5mm', arc),
            ('line, travel, line', [(60, 0, v, 1), (0, 20, 150, 0), (-60, 0, v, 1)])]

def run(K, **kw):
    setup(K, **kw)
    rows = []
    for name, p in paths(sim.SPEED):
        st, log, end_t = sim.simulate(p)
        diff = st['e_pos'] - st['nominal']
        track = max(abs(e - n) for _, e, n, _ in log) if log else 0
        rates = st.get('rates', [0])
        rows.append((name, st['nominal'], diff, track, max(rates), min(rates)))
    return rows

if __name__ == '__main__':
    for K, kw in [(0.0, {}), (0.0, {'scurve': True}),
                  (0.6, {}), (0.6, {'scurve': True}),
                  (0.6, {'scurve': True, 'J': 1.2, 'U': 0.06, 'R': 20}),
                  (0.6, {'scurve': True, 'A': 1000}),
                  (0.6, {'scurve': True, 'A': 1000, 'V': 25}),
                  (0.6, {'scurve': True, 'A': 3000, 'V': 25}),
                  (0.6, {'scurve': True, 'U': 0.05})]:
        tag = f"K{K} " + ' '.join(f"{k}={v}" for k, v in kw.items())
        print(f"== {tag}")
        for name, nom, diff, track, mx, mn in run(K, **kw):
            print(f"  {name:22s} E steps {nom:6d}  end diff {diff:+3d}  max |emitted-nominal| {track:4d}  E rate {mn:+6d}..{mx:+6d} steps/s ({mn/415:+5.1f}..{mx/415:+5.1f} mm/s)")
