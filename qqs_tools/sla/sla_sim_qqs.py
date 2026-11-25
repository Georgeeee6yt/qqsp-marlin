#!/usr/bin/env python3
"""
QQS-Pro v3.2.9: George's B1 sla_sim_v54.py + S-curve (Bezier) in the stepper and in the lookahead,
exactly as the firmware does it (Marlin's portable _eval_bezier_curve, get_period_inverse,
"t < T ? eval : end rate"). SCURVE=1 env var turns it on.

Host simulation of the V5 Smooth LA port (rate mode, no E sync), mirroring the
integer math and ISR scheduling in stepper.cpp, to measure:
  - total E steps emitted vs. nominal (drift / under-extrusion)
  - idle creep after motion stops
  - when the pressure release starts relative to XY deceleration
Usage: sla_sim.py [K] [tau] [speed] [ed_ratio]
"""
import math, sys, os
SCURVE = os.environ.get("SCURVE", "0") == "1"

def bezier(v0, v1, av, t, T):
    """Marlin stepper.cpp portable _calc_bezier_curve_coeffs + _eval_bezier_curve, with the t >= T guard."""
    if t >= T: return v1
    A, B, C, F = 768 * (v1 - v0), 1920 * (v0 - v1), 1280 * (v1 - v0), 128 * v0
    tt = (av * t) & 0xFFFFFFFF
    f = tt; f *= tt; f >>= 32; f *= tt; f >>= 32
    acc = F << 31
    acc += ((f & 0xFFFFFFFF) >> 1) * C
    f *= tt; f >>= 32
    acc += ((f & 0xFFFFFFFF) >> 1) * B
    f *= tt; f >>= 32
    acc += ((f & 0xFFFFFFFF) >> 1) * A
    acc >>= (31 + 7)
    return max(int(acc), 0)

def period_inverse(d):
    return 0xFFFFFFFF // d if d else 0xFFFFFFFF

TIMER = 2_000_000
HZ = 1000
INTERVAL = TIMER // HZ
ORDER = 5
MINRATE = 120
POS = False
ALPHA = 0.0
RELAX_MS = 150
J = 0.0
TAU_B = 0.01
TAU_U = 0.02
V52 = False
HOLD = False
LIN = False
R_MMS = 20.0
E_ACC = 0.0
E_VMAX = 0.0   # QQS-Pro M900 V (mm/s, 0 = off)
CORR = 20
ESTEPS = 415.0
XYSTEPS = 80.0
NEVER = 0xFFFFFFFF

K = float(sys.argv[1]) if len(sys.argv) > 1 else 0.65
TAU = float(sys.argv[2]) if len(sys.argv) > 2 else 0.02
SPEED = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
ED = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0338   # filament mm per path mm
ACCEL = 700.0
JD = 0.05

def mulq(q, x, y):
    p = int(x) * int(y)
    return p >> q if p >= 0 else -((-p) >> q) if False else (p >> q)  # python >> on negative = floor, same as ARM asr

class Block:
    pass

def make_blocks(path):
    """path: list of (dx, dy, speed, extrude) segments. Returns planned blocks."""
    blocks = []
    for (dx, dy, v, ext) in path:
        b = Block()
        mm = math.hypot(dx, dy)
        b.sx, b.sy = round(abs(dx) * XYSTEPS), round(abs(dy) * XYSTEPS)
        b.se = round(mm * ED * ESTEPS) if ext else 0
        b.sec = max(b.sx, b.sy, b.se)
        b.mm = mm
        b.dirv = (dx / mm, dy / mm)
        b.spmm = b.sec / mm
        b.nominal_rate = math.ceil(v * b.spmm)
        b.accel_steps = math.ceil(ACCEL * b.spmm)
        b.acceleration_rate = int(b.accel_steps * (float(1 << 24) / TIMER))
        b.v = v
        b.use_lead = bool(ext) and K > 0
        ratio = (b.se << 30) // b.sec
        b.e_ratio_q30 = ratio
        blocks.append(b)
    # junction speeds (JD), simple forward/backward pass
    n = len(blocks)
    vj = [0.0] * (n + 1)
    for i in range(1, n):
        a, c = blocks[i - 1].dirv, blocks[i].dirv
        cos_t = -(a[0] * c[0] + a[1] * c[1])
        if cos_t > 0.999999: vmax = 0.0
        elif cos_t < -0.999999: vmax = min(blocks[i-1].v, blocks[i].v)
        else:
            sin_half = math.sqrt(0.5 * (1 - cos_t))
            vmax = math.sqrt(ACCEL * JD * sin_half / (1 - sin_half))
        vj[i] = min(vmax, blocks[i - 1].v, blocks[i].v)
    for i in range(n - 1, -1, -1):   # backward
        vj[i] = min(vj[i], math.sqrt(vj[i + 1] ** 2 + 2 * ACCEL * blocks[i].mm))
    for i in range(n):               # forward
        vj[i + 1] = min(vj[i + 1], math.sqrt(vj[i] ** 2 + 2 * ACCEL * blocks[i].mm))
    for i, b in enumerate(blocks):
        ir = max(math.ceil(vj[i] * b.spmm), MINRATE)
        fr = max(math.ceil(vj[i + 1] * b.spmm), MINRATE)
        acc = b.accel_steps
        inv = 1.0 / acc
        nr2 = float(b.nominal_rate) ** 2
        dsf = 0.5 * inv * (nr2 - fr * fr)
        asf = 0.5 * inv * (nr2 - ir * ir)
        a_st, d_st = math.ceil(asf), math.floor(dsf)
        plateau = b.sec - a_st - d_st
        cruise = b.nominal_rate
        if plateau < 0:
            a_st = min(max(int(math.ceil((b.sec + asf - dsf) * 0.5)), 0), b.sec)
            d_st = b.sec - a_st
            cruise = int(math.sqrt(ir * ir + 2 * acc * a_st))
        b.initial_rate, b.final_rate, b.cruise_rate = ir, fr, cruise
        b.accelerate_until, b.decelerate_after = a_st, b.sec - d_st
        rf = inv * TIMER
        b.acceleration_time = int(rf * max(0.0, cruise - ir))
        b.deceleration_time = int(rf * max(0.0, cruise - fr))
        b.cruise_time = int(plateau * TIMER / cruise) if plateau > 0 else 0
        b.acc_inv = period_inverse(b.acceleration_time)
        b.dec_inv = period_inverse(b.deceleration_time)
    return blocks

def step_multiply(t, rate):
    return (t * rate) >> 24

def simulate(path, idle_ms=300):
    blocks = make_blocks(path)
    tau_ticks = int(TAU * TIMER)
    alpha = 1.0 - math.exp(-INTERVAL * ORDER / tau_ticks) if tau_ticks else 1.0
    alpha_q30 = int(alpha * (1 << 30))
    k_q27 = int(K * (1 << 27))

    st = dict(cur=0, curr_tick=0, curr_rate=0, last_target=0, smoothed=[0] * ORDER,
              la_interval=NEVER, next_adv=NEVER, e_dir=1, e_pos=0, nominal=0, rates=[])
    queue = list(blocks)

    def lookahead(ticks):
        for b in queue:
            if ticks <= b.acceleration_time:
                if not b.use_lead: return 0
                if SCURVE: r = bezier(b.initial_rate, b.cruise_rate, b.acc_inv, ticks, b.acceleration_time)
                else: r = min(step_multiply(ticks, b.acceleration_rate) + b.initial_rate, b.cruise_rate)
                return mulq(30, r, b.e_ratio_q30)
            ticks -= b.acceleration_time
            if ticks <= b.cruise_time:
                return mulq(30, b.cruise_rate, b.e_ratio_q30) if b.use_lead else 0
            ticks -= b.cruise_time
            if ticks <= b.deceleration_time:
                if not b.use_lead: return 0
                if SCURVE: r = bezier(b.cruise_rate, b.final_rate, b.dec_inv, ticks, b.deceleration_time)
                else:
                    r = step_multiply(ticks, b.acceleration_rate)
                    r = max(b.cruise_rate - r, b.final_rate) if r < b.cruise_rate else b.final_rate
                return mulq(30, r, b.e_ratio_q30)
            ticks -= b.deceleration_time
        return 0

    def set_la(rate):
        rate = max(-40000, min(40000, rate))
        if E_ACC > 0:
            mx = int(E_ACC * ESTEPS / HZ)
            prev = st.get('last_rate', 0)
            rate = max(prev - mx, min(prev + mx, rate))
        if E_VMAX > 0:   # QQS-Pro M900 V
            vm = int(E_VMAX * ESTEPS)
            rate = max(-vm, min(vm, rate))
        st['last_rate'] = rate
        if rate == 0:
            st['la_interval'] = NEVER; return
        st['e_dir'] = -1 if rate < 0 else 1
        st['la_interval'] = TIMER // abs(rate)

    def smooth_isr():
        cur = queue[0] if queue else None
        if cur:
            if V52:
                eb = lookahead(int(TAU_B * TIMER) + st['curr_tick'])
                eu = lookahead(int(TAU_U * TIMER) + st['curr_tick'])
                target = mulq(27, min(eb, eu), k_q27)
            else:
                target = mulq(27, lookahead(tau_ticks + st['curr_tick']), k_q27)
        else:
            st['curr_rate'] = 0; target = 0
        planned = mulq(30, st['curr_rate'], cur.e_ratio_q30) if cur else 0
        if POS:
            # release-only gain: extra (Q8) gets alpha * every target decrease, then relaxes to 0
            d = target - st.setdefault('raw_prev', 0)
            st['raw_prev'] = target
            ex = st.setdefault('extra', 0)
            decay = int(math.exp(-INTERVAL / (RELAX_MS * TIMER / 1000.0)) * (1 << 30))
            pk = st.setdefault('peak', 0)
            if target > pk: pk = target
            elif cur and cur.use_lead and (d >= 0 or not HOLD): pk = mulq(30, pk, decay)
            st['peak'] = pk
            a_rel = ((J / K - 1.0) if (V52 and J > 0 and K > 0) else (0.0 if V52 else ALPHA))
            if d < 0: ex += int(a_rel * 256) * d
            if cur and getattr(cur, 'e_only_fwd', False): ex = 0   # unretract / prime: give the extra release back now
            if cur and cur.use_lead and (d >= 0 or not HOLD):
                if LIN:
                    stp = int(R_MMS * ESTEPS * 256 / HZ)
                    ex = min(ex + stp, 0) if ex < 0 else max(ex - stp, 0)
                else:
                    ex = mulq(30, ex, decay)
            cap = abs(int(a_rel * 256)) * pk
            ex = max(min(ex, cap), -cap)
            st['extra'] = ex
            st.setdefault('extras', []).append(ex)
            s = target * 256 + ex
            for i in range(ORDER):
                if V52:
                    tt = TAU_B if s > st['smoothed'][i] else TAU_U
                    a = int((1.0 - math.exp(-INTERVAL * ORDER / (tt * TIMER))) * (1 << 30)) if tt > 0 else (1 << 30)
                else:
                    a = alpha_q30
                st['smoothed'][i] += mulq(30, s - st['smoothed'][i], a)
                s = st['smoothed'][i]
            p_rate = ((s - st['last_target']) * HZ + 128) >> 8
            st['last_target'] = s
            err = (st['nominal'] * 256 + s) - st['e_pos'] * 256
            if err > 256: corr = ((err - 256) * HZ) // (256 * CORR)
            elif err < -256: corr = -(((-err - 256) * HZ) // (256 * CORR))
            else: corr = 0
            set_la(planned + p_rate + corr)
            st['rates'].append(st['last_rate'])   # QQS-Pro: log what was applied (after A / V limits)
        else:
            la = (target - st['last_target']) * HZ
            st['last_target'] = target
            lim = (2**31 - 1) >> 9
            s = max(-lim, min(lim, la)) * 256
            for i in range(ORDER):
                st['smoothed'][i] += mulq(30, s - st['smoothed'][i], alpha_q30)
                s = st['smoothed'][i]
            la = (s + 128) >> 8
            set_la(la + planned)
            st['rates'].append(st['last_rate'])
        st['curr_tick'] += INTERVAL
        return INTERVAL

    # Main loop (event driven on ticks)
    t = 0
    next_main = 0
    next_smooth = 0
    # block execution state
    b = None; step_done = 0; acc_time = dec_time = 0; acc_rate = 0; e_err = 0
    log = []
    end_t = None
    while True:
        if b is None and queue:
            b = queue[0]; step_done = 0; acc_time = dec_time = 0
            st['curr_tick'] = 0; st['curr_rate'] = b.initial_rate; acc_rate = b.initial_rate
            e_err = -b.sec
        if not queue and end_t is None:
            end_t = t
        if end_t is not None and t > end_t + idle_ms * TIMER // 1000:
            break
        # pick the next event
        dt = min(next_main, next_smooth, st['next_adv'])
        t += dt
        next_main -= dt; next_smooth -= dt
        if st['next_adv'] != NEVER: st['next_adv'] -= dt
        if st['next_adv'] == 0:
            st['e_pos'] += st['e_dir']
            st['next_adv'] = st['la_interval']
        elif st['next_adv'] > st['la_interval']:
            st['next_adv'] = st['la_interval']
        if next_main == 0:
            if b is not None:
                # pulse phase: nominal E Bresenham
                e_err += 2 * b.se
                if e_err >= 0:
                    e_err -= 2 * b.sec; st['nominal'] += (-1 if b.e_ratio_q30 < 0 else 1)
                step_done += 1
                if step_done >= b.sec:
                    queue.pop(0); b = None
                    if queue:   # Marlin loads the next block in the same block_phase_isr call
                        b = queue[0]; step_done = 0; acc_time = dec_time = 0
                        st['curr_tick'] = 0; st['curr_rate'] = b.initial_rate; acc_rate = b.initial_rate
                        e_err = -b.sec
                        iv = TIMER // b.initial_rate; acc_time += iv
                        next_main = iv
                    else:
                        next_main = INTERVAL
                else:
                    if step_done <= b.accelerate_until:
                        if SCURVE: acc_rate = bezier(b.initial_rate, b.cruise_rate, b.acc_inv, acc_time, b.acceleration_time) if acc_time < b.acceleration_time else b.cruise_rate
                        else: acc_rate = min(step_multiply(acc_time, b.acceleration_rate) + b.initial_rate, b.nominal_rate)
                        acc_rate = max(acc_rate, MINRATE)
                        iv = TIMER // acc_rate; acc_time += iv; st['curr_rate'] = acc_rate
                    elif step_done > b.decelerate_after:
                        if SCURVE:
                            if not getattr(b, 'second_half', False): b.second_half = True; r = b.cruise_rate
                            else: r = bezier(b.cruise_rate, b.final_rate, b.dec_inv, dec_time, b.deceleration_time) if dec_time < b.deceleration_time else b.final_rate
                        else:
                            r = step_multiply(dec_time, b.acceleration_rate)
                            r = max(acc_rate - r, b.final_rate) if r < acc_rate else b.final_rate
                        r = max(r, MINRATE)
                        iv = TIMER // r; dec_time += iv; st['curr_rate'] = r
                    else:
                        iv = TIMER // b.nominal_rate; st['curr_rate'] = b.nominal_rate
                        acc_rate = b.nominal_rate
                    next_main = iv
            else:
                next_main = INTERVAL
        if next_smooth == 0:
            next_smooth = smooth_isr()
            log.append((t, st['e_pos'], st['nominal'], st['la_interval']))
            if st['next_adv'] > st['la_interval']:
                st['next_adv'] = st['la_interval']
    return st, log, end_t

def square(side=40, v=SPEED):
    return [(side, 0, v, 1), (0, side, v, 1), (-side, 0, v, 1), (0, -side, v, 1)]

if __name__ == '__main__':
    for name, path in [('square 40mm', square()),
                       ('long line 150mm', [(150, 0, SPEED, 1)]),
                       ('slow 5mm/s line 20mm', [(20, 0, 5.0, 1)]),
                       ('short segs (arc-ish) 60x1mm', [(math.cos(i*0.1), math.sin(i*0.1), SPEED, 1) for i in range(60)])]:
        st, log, end_t = simulate(path)
        nominal, emitted = st['nominal'], st['e_pos']
        err = emitted - nominal
        print(f"{name:28s} K={K} tau={TAU}: nominal E steps {nominal:6d}, emitted {emitted:6d}, "
              f"diff {err:+d} ({100.0*err/max(nominal,1):+.2f}%), final rate {'idle' if st['la_interval']==NEVER else st['la_interval']}")
