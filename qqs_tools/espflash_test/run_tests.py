#!/usr/bin/env python3
"""
Runs the QQS-Pro ESP flasher core (host build) against fake_esp.py, normal flows and faults.
  python3 run_tests.py <host_test binary> <ESPSTUB.BIN> <stub json> <MksWifi.bin> <workdir>
"""
import hashlib, os, random, shutil, signal, subprocess, sys, time

host, stubbin, stubjson, mkswifi, work = sys.argv[1:6]
here = os.path.dirname(os.path.abspath(__file__))
fails = 0


def run(name, op, sdfiles, flash, faults=(), expect_err=0, check=None, env=None):
    global fails
    d = os.path.join(work, name)
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d + '/sd')
    for fn, data in sdfiles.items():
        open(os.path.join(d, 'sd', fn), 'wb').write(data)
    open(d + '/flash.bin', 'wb').write(flash)
    fifo = d + '/ctl'; os.mkfifo(fifo)
    args = [sys.executable, here + '/fake_esp.py', '--flash', d + '/flash.bin', '--ctl', fifo,
            '--stubjson', stubjson, '--log', d + '/fake.log']
    for f in faults: args += ['--fault', f]
    fake = subprocess.Popen(args, stdout=subprocess.PIPE, text=True)
    pty = fake.stdout.readline().strip()
    t0 = time.time()
    r = subprocess.run([host, pty, fifo, d + '/sd', op], capture_output=True, text=True,
                       env=dict(os.environ, **(env or {})), timeout=600)
    dt = time.time() - t0
    fake.send_signal(signal.SIGINT); fake.wait(5)
    out = open(d + '/flash.bin.out', 'rb').read()
    res = [l for l in r.stdout.splitlines() if l.startswith('RESULT')]
    res = res[0] if res else '(no result) ' + r.stderr[-500:]
    kv = dict(x.split('=', 1) for x in res.split()[1:] if '=' in x)
    ok = int(kv.get('err', -1)) == expect_err and kv.get('sd_balanced') == '1'
    if ok and check:
        msg = check(d, out, kv)
        if msg: ok = False; res += '  CHECK: ' + msg
    if 'AddressSanitizer' in r.stderr or 'runtime error' in r.stderr:
        ok = False; res += '  SANITIZER: ' + r.stderr[-800:]
    print('%-4s %-30s %5.1fs  %s' % ('ok' if ok else 'FAIL', name, dt, res))
    if not ok:
        fails += 1
        print(r.stderr[-1500:])
    return d, out, kv


random.seed(7)
flash = bytes(random.getrandbits(8) for _ in range(4 << 20))
stub = open(stubbin, 'rb').read()
mks = open(mkswifi, 'rb').read()
md5 = lambda b: hashlib.md5(b).hexdigest()


def chk_backup(name):
    def c(d, out, kv):
        b = open(os.path.join(d, 'sd', name), 'rb').read()
        if b != flash: return 'backup differs from flash'
        if out != flash: return 'flash changed by a backup'
    return c


# --- normal flows ---
run('backup', 'backup', {'ESPSTUB.BIN': stub}, flash, check=chk_backup('ESPBAK.BIN'))
run('backup_second_name', 'backup', {'ESPSTUB.BIN': stub, 'espbak.bin': b'x'}, flash, check=chk_backup('ESPBAK1.BIN'))
full = {'ESPSTUB.BIN': stub, 'ESPBAK.BIN': b'1'}
full.update({'ESPBAK%d.BIN' % i: b'1' for i in range(1, 10)})
run('backup_all_names_taken', 'backup', full, flash, expect_err=6)

newimg = bytes(random.getrandbits(8) for _ in range(4 << 20))
run('restore', 'restore', {'ESPSTUB.BIN': stub, 'ESPBAK.BIN': newimg}, flash,
    check=lambda d, out, kv: None if out == newimg else 'flash != backup file')


def chk_fw(d, out, kv):
    e = (len(mks) + 4095) // 4096 * 4096
    if out[:len(mks)] != mks: return 'image not in flash'
    if out[len(mks):e] != b'\xff' * (e - len(mks)): return 'rest of last sector not erased'
    if out[e:] != flash[e:]: return 'flash beyond the image changed'
    names = sorted(os.listdir(d + '/sd'))
    if 'MKS_WIFI_CUR' not in names or any(n.lower() == 'mkswifi.bin' for n in names): return 'not renamed: %s' % names


run('fw_update', 'fw', {'ESPSTUB.BIN': stub, 'MksWifi.bin': mks, 'MKS_WIFI_CUR': b'old'}, flash, check=chk_fw)
run('fw_update_lowercase_name', 'fw', {'ESPSTUB.BIN': stub, 'mkswifi.bin': mks}, flash, check=chk_fw)

web = bytes(random.getrandbits(8) for _ in range(0x2FB000))


def chk_web(d, out, kv):
    if out[0x100000:0x3FB000] != web: return 'web image not at 0x100000'
    if out[:0x100000] != flash[:0x100000] or out[0x3FB000:] != flash[0x3FB000:]: return 'outside SPIFFS changed'
    if 'MKS_WEB_CONTROL_CUR' not in os.listdir(d + '/sd'): return 'not renamed'


run('web_update_full_spiffs', 'web', {'ESPSTUB.BIN': stub, 'MksWifi_WebView.bin': web}, flash, check=chk_web)

# --- refusals before touching the module ---
run('no_stub', 'fw', {'MksWifi.bin': mks}, flash, expect_err=2)
run('stub_not_qstb', 'fw', {'ESPSTUB.BIN': b'X' * 100, 'MksWifi.bin': mks}, flash, expect_err=2)
run('no_image', 'fw', {'ESPSTUB.BIN': stub}, flash, expect_err=3)
run('fw_not_esp_image', 'fw', {'ESPSTUB.BIN': stub, 'MksWifi.bin': b'\x00' * 1000}, flash, expect_err=5)
run('fw_too_big', 'fw', {'ESPSTUB.BIN': stub, 'MksWifi.bin': b'\xe9' + b'\0' * 0x100000}, flash, expect_err=4)
run('web_too_big', 'web', {'ESPSTUB.BIN': stub, 'MksWifi_WebView.bin': b'\0' * (0x2FB000 + 1)}, flash, expect_err=4)
run('restore_bigger_than_chip', 'restore', {'ESPSTUB.BIN': stub, 'ESPBAK.BIN': b'\0' * (8 << 20)}, flash, expect_err=4,
    check=lambda d, out, kv: None if out == flash else 'flash touched')
run('no_sd', 'fw', {'ESPSTUB.BIN': stub, 'MksWifi.bin': mks}, flash, expect_err=1, env={'NOSD': '1'})

# --- link / module faults ---
run('no_sync', 'fw', {'ESPSTUB.BIN': stub, 'MksWifi.bin': mks}, flash, faults=['nosync'], expect_err=7,
    check=lambda d, out, kv: None if out == flash else 'flash touched')
run('boot_noise_with_C0', 'fw', {'ESPSTUB.BIN': stub, 'MksWifi.bin': mks}, flash, faults=['noise'], check=chk_fw)
run('baud_921600_fails_falls_back', 'fw', {'ESPSTUB.BIN': stub, 'MksWifi.bin': mks}, flash, faults=['deaf921600'],
    check=lambda d, out, kv: chk_fw(d, out, kv) or (None if kv['baud'] == '460800' else 'baud %s' % kv['baud']))
rb = lambda n: (lambda d, out, kv: chk_backup('ESPBAK.BIN')(d, out, kv) or (None if int(kv['retries']) >= n else 'retries %s' % kv['retries']))
run('read_bitflip_reread', 'backup', {'ESPSTUB.BIN': stub}, flash, faults=['readflip'], check=rb(1))
run('read_bitflip_always', 'backup', {'ESPSTUB.BIN': stub}, flash, faults=['readflipalways'], expect_err=14)
run('read_lost_end_c0', 'backup', {'ESPSTUB.BIN': stub}, flash, faults=['dropend'], check=rb(1))
run('read_lost_byte', 'backup', {'ESPSTUB.BIN': stub}, flash, faults=['dropbyte'], check=rb(1))
run('read_lost_packet', 'backup', {'ESPSTUB.BIN': stub}, flash, faults=['droppacket'], check=rb(1))
run('read_lost_ack', 'backup', {'ESPSTUB.BIN': stub}, flash, faults=['dropack'], check=rb(1))
run('read_all_four_losses', 'backup', {'ESPSTUB.BIN': stub}, flash, faults=['dropend', 'dropbyte', 'droppacket', 'dropack', 'readflip'], check=rb(5))
run('write_error_reported_late', 'fw', {'ESPSTUB.BIN': stub, 'MksWifi.bin': mks}, flash, faults=['writeerr'], expect_err=11,
    check=lambda d, out, kv: None if kv['code'] == '0xc400' else 'code %s' % kv['code'])
run('flash_content_wrong_md5', 'fw', {'ESPSTUB.BIN': stub, 'MksWifi.bin': mks}, flash, faults=['badmd5'], expect_err=14,
    check=lambda d, out, kv: None if 'MksWifi.bin' in os.listdir(d + '/sd') else 'renamed although not verified')
run('slow_erase', 'fw', {'ESPSTUB.BIN': stub, 'MksWifi.bin': mks}, flash, faults=['slowerase'], check=chk_fw)

print('\n%s' % ('ALL PASSED' if not fails else '%d FAILED' % fails))
sys.exit(1 if fails else 0)
