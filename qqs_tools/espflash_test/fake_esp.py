#!/usr/bin/env python3
"""
Fake ESP8266 for testing the QQS-Pro flasher (and checked against real esptool first).

Speaks the serial protocol of the ESP8266 ROM bootloader and of esp-flasher-stub 2.0.0 as found in their
sources (esptool loader.py, esp-flasher-stub command_handler.c): SLIP frames, 2-byte big-endian status,
OHAI after the stub upload, READ_FLASH with one packet in flight + ACKs + MD5, FLASH_DATA written after
the answer (a write error is reported on the next command).

  fake_esp.py --flash flash.bin [--ctl FIFO] [--fault NAME]... -> prints the pty path, then serves.

Control FIFO lines (the test HAL's "pins"): "reset 1" = reset with GPIO0 low (ROM download mode),
"reset 0" = normal boot (app: sends one MKS status packet). Without --ctl the chip starts in the ROM.

Faults: deaf921600 (no answers after switching to 921600), nosync, noise (boot text with C0 bytes before
the ROM listens), readflip (one byte of a READ_FLASH packet flipped on the wire), writeerr (write error on
block 3 reported with the next command), badmd5 (flash content wrong after write), slowerase.
"""
import argparse, hashlib, os, pty, select, struct, sys, time, tty, base64, json

p = argparse.ArgumentParser()
p.add_argument('--flash', required=True)
p.add_argument('--ctl')
p.add_argument('--stubjson', default='')
p.add_argument('--fault', action='append', default=[])
p.add_argument('--flash-id', type=lambda x: int(x, 0), default=0x1640EF)   # Winbond W25Q32: EF 40 16
p.add_argument('--log', default='')
a = p.parse_args()

flash = bytearray(open(a.flash, 'rb').read())
log = open(a.log, 'a') if a.log else None
def L(*s):
    if log: log.write(' '.join(str(x) for x in s) + '\n'); log.flush()

master, slave = pty.openpty()
tty.setraw(master); tty.setraw(slave)
print(os.ttyname(slave), flush=True)
ctl = os.open(a.ctl, os.O_RDONLY | os.O_NONBLOCK) if a.ctl else None

def w(b):
    while b:
        n = os.write(master, b); b = b[n:]

def slip(b):
    w(b'\xc0' + b.replace(b'\xdb', b'\xdb\xdd').replace(b'\xc0', b'\xdb\xdc') + b'\xc0')

def resp(op, val=0, data=b'', status=0):
    body = data + struct.pack('>H', status)
    slip(struct.pack('<BBHI', 1, op, len(body), val) + body)

mode = 'rom' if not ctl else 'off'
baud = 115200
deaf = False
mem = {}            # addr -> bytes uploaded to RAM
wire_hits = {}      # FLASH_DATA seq -> times damaged on the wire
mem_cur = None
regs = {}
flash_size_set = 0
fl = None           # flash write state
pending_err = 0
erased = 0
stats = dict(sync=0, ohai=0, md5=0, reads=0, writes=0)
expected_stub = None
if a.stubjson:
    j = json.load(open(a.stubjson))
    expected_stub = {j['text_start']: base64.b64decode(j['text']), j['data_start']: base64.b64decode(j['data'])}

rx = bytearray(); in_frame = False; esc = False; frames = []
def feed(data):
    global in_frame, esc, rx
    for c in data:
        if c == 0xC0:
            if in_frame and rx: frames.append(bytes(rx))
            in_frame = True; rx = bytearray(); esc = False; continue
        if not in_frame: continue
        if esc:
            esc = False; c = {0xDC: 0xC0, 0xDD: 0xDB}.get(c, c)
        elif c == 0xDB: esc = True; continue
        rx.append(c)

def read_frame(timeout=5.0):
    end = time.time() + timeout
    while not frames:
        r, _, _ = select.select([master], [], [], max(0, end - time.time()))
        if not r: return None
        feed(os.read(master, 65536))
    return frames.pop(0)

_once = set()
def once(k):
    if k in _once: return False
    _once.add(k); return True

def boot_app():
    # MKS status packet (type 0): ip 192.168.1.117, port 8080, connected, STA, ssid, key
    ss, key = b'TestNet', b'pass1234'
    d = bytes([192, 168, 1, 117]) + struct.pack('<H', 8080) + bytes([0x0A, 2, len(ss)]) + ss + bytes([len(key)]) + key + b'\0\0\0\0\x14' + b'A' * 20 + b'\x051.0.4'
    w(b'\xa5\x00' + struct.pack('<H', len(d)) + d + b'\xfc')

def handle_ctl():
    global mode, baud, deaf, mem, regs, fl, pending_err
    try: data = os.read(ctl, 1000).decode()
    except BlockingIOError: return
    for line in data.split('\n'):
        if line.startswith('reset'):
            boot = line.split()[1] == '1'
            L('ctl', line)
            mode = 'rom' if boot else 'app'
            baud = 115200; deaf = False; mem = {}; fl = None; pending_err = 0
            frames.clear()
            if 'noise' in a.fault:   # ROM boot text at 74880 looks like garbage, may hold C0 / DB
                w(bytes([0x00, 0xC0, 0x12, 0xDB, 0x55, 0xC0, 0xFF, 0x01, 0xC0]) + os.urandom(40))
            if not boot:
                time.sleep(0.3); boot_app()

def spi_reg_write(addr, val):
    regs[addr] = val
    if addr == 0x60000200 and val & (1 << 18):           # SPI_CMD_USR: run the user command
        cmd = regs.get(0x60000224, 0) & 0xFF
        assert regs.get(0x60000224, 0) >> 28 == 7, 'command length must be 8 bits'
        if cmd == 0x9F:
            assert regs.get(0x6000021C, 0) & (1 << 28), 'MISO phase must be on'
            assert (regs.get(0x60000220, 0) >> 8) & 0x1FF == 23, 'MISO bit length 24'
            regs[0x60000240] = a.flash_id
        regs[0x60000200] = 0                              # done at once

def rom_read_reg(addr):
    return {0x40001000: 0xFFF0C101, 0x3FF00050: 0x12345678, 0x3FF00054: 0x0000ABCD, 0x3FF00058: 0, 0x3FF0005C: 0}.get(addr, regs.get(addr, 0))

def handle(f):
    global mode, baud, deaf, mem_cur, flash_size_set, fl, pending_err, erased
    if len(f) < 8 or f[0] != 0: L('ignored frame', f[:12].hex()); return
    op, ln, chk = f[1], struct.unpack('<H', f[2:4])[0], struct.unpack('<I', f[4:8])[0]
    d = f[8:]
    if len(d) != ln: L('length mismatch', op, ln, len(d)); return
    if deaf: L('deaf, ignoring', hex(op)); return
    stub = mode == 'stub'
    def ok(val=0, data=b''):
        global pending_err
        e = pending_err; pending_err = 0
        resp(op, val, data if not e else b'', e)
    L('cmd', hex(op), ln, mode)
    if op == 0x08:
        if 'nosync' in a.fault: return
        if d != b'\x07\x07\x12\x20' + b'\x55' * 32: return
        stats['sync'] += 1
        for _ in range(8): resp(op, 0 if stub else 0x20120707)   # ROM and stub both answer 8 times
    elif op == 0x0A:
        addr = struct.unpack('<I', d)[0]
        ok(rom_read_reg(addr) if addr not in regs or addr == 0x40001000 else regs[addr])
    elif op == 0x09:
        assert len(d) % 16 == 0
        for i in range(0, len(d), 16):
            addr, val, mask, dly = struct.unpack('<IIII', d[i:i + 16])
            spi_reg_write(addr, val)
        ok()
    elif op == 0x05:                                     # MEM_BEGIN
        size, blocks, bs, addr = struct.unpack('<IIII', d)
        mem_cur = [addr, size, blocks, bs, bytearray(), 0]
        ok()
    elif op == 0x07:                                     # MEM_DATA
        n, seq = struct.unpack('<II', d[:8]); data = d[16:]
        c = 0xEF
        for b in data: c ^= b
        if c != chk or n != len(data) or seq != mem_cur[5]:
            L('MEM_DATA bad', c, chk, n, len(data), seq); resp(op, 0, b'', 0x0107); return
        mem_cur[4] += data; mem_cur[5] += 1
        if len(mem_cur[4]) == mem_cur[1]: mem[mem_cur[0]] = bytes(mem_cur[4])
        ok()
    elif op == 0x06:                                     # MEM_END
        flag, entry = struct.unpack('<II', d)
        ok()
        if expected_stub is not None:
            for addr, data in expected_stub.items():
                if mem.get(addr) != data: L('STUB MISMATCH at', hex(addr)); return
        mode = 'stub'; stats['ohai'] += 1
        time.sleep(0.02)
        slip(b'OHAI')
    elif not stub:
        resp(op, 0, b'', 0x0105)                         # ROM: invalid message (we only test via stub)
    elif op == 0x0F:                                     # CHANGE_BAUDRATE: answer at the old speed, then switch
        new, old = struct.unpack('<II', d)
        if old != baud: L('CHANGE_BAUDRATE old', old, 'but link at', baud, '(pty: no real baud)')
        ok(); baud = new
        if 'deaf921600' in a.fault and new == 921600: deaf = True
    elif op == 0x0B:
        fid, size, blk, sec, page, mask = struct.unpack('<6I', d)
        flash_size_set = size; ok()
    elif op == 0xD1:                                     # ERASE_REGION (answers when done)
        addr, size = struct.unpack('<II', d)
        if addr % 4096 or size % 4096 or addr + size > len(flash): resp(op, 0, b'', 0xC000); return
        if 'slowerase' in a.fault: time.sleep(2)
        flash[addr:addr + size] = b'\xff' * size; erased += size; ok()
    elif op == 0x02:                                     # FLASH_BEGIN
        if len(d) not in (16, 20): resp(op, 0, b'', 0xC000); return
        size, blocks, bs, off = struct.unpack('<IIII', d[:16])
        fl = dict(rem=size, off=off, seq=0, bs=bs)
        ok()
    elif op == 0x03:                                     # FLASH_DATA: answer, then write
        n, seq = struct.unpack('<II', d[:8]); data = d[16:]
        # wire damage the stub sees: 'wireflip' = blocks 2 and 5 arrive damaged once (5: twice),
        # 'wireflipalways' = block 2 always arrives damaged
        if ('wireflip' in a.fault and wire_hits.get(seq, 0) < {2: 1, 5: 2}.get(seq, 0)) or \
           ('wireflipalways' in a.fault and seq == 2):
            wire_hits[seq] = wire_hits.get(seq, 0) + 1
            data = bytearray(data); data[100] ^= 0x10; data = bytes(data)
        c = 0xEF
        for b in data: c ^= b
        if n != len(data): resp(op, 0, b'', 0xC900); return
        if c != chk: resp(op, 0, b'', 0xC100); return
        assert seq == fl['seq'], (seq, fl['seq'])
        ok()
        k = min(len(data), fl['rem'])
        chunk = bytearray(data[:k])
        if 'badmd5' in a.fault and seq == 1: chunk[5] ^= 0x40
        if 'writeerr' in a.fault and seq == 3: pending_err = 0xC400
        flash[fl['off']:fl['off'] + k] = chunk
        fl['off'] += k; fl['rem'] -= k; fl['seq'] += 1; stats['writes'] += 1
    elif op == 0x04:                                     # FLASH_END
        if pending_err: ok(); return
        if fl is None or fl['rem'] != 0: resp(op, 0, b'', 0xC000); return
        fl = None; ok()
    elif op == 0x13:                                     # SPI_FLASH_MD5 -> 16 raw bytes
        addr, size, _, _ = struct.unpack('<IIII', d)
        stats['md5'] += 1
        ok(0, hashlib.md5(flash[addr:addr + size]).digest())
    elif op == 0xD2:                                     # READ_FLASH
        off, size, pk, inflight = struct.unpack('<IIII', d)
        if pk > 4096 or (size and not pk): resp(op, 0, b'', 0xC000); return
        ok()
        stats['readcmds'] = stats.get('readcmds', 0) + 1
        sent = 0; h = hashlib.md5()
        while sent < size:
            chunk = bytes(flash[off + sent:off + sent + min(pk, size - sent)])
            at = off + sent
            h.update(chunk)                              # the stub hashes what it reads
            wire = chunk
            if 'readflip' in a.fault and 8192 <= at and once('readflip'):
                wire = bytearray(chunk); wire[100] ^= 1; wire = bytes(wire)
            if 'readflipalways' in a.fault and at == 8192:
                wire = bytearray(chunk); wire[100] ^= 1; wire = bytes(wire)
            enc = b'\xc0' + wire.replace(b'\xdb', b'\xdb\xdd').replace(b'\xc0', b'\xdb\xdc') + b'\xc0'
            if 'dropend' in a.fault and at >= 0x40000 and once('dropend'): enc = enc[:-1]          # lost closing C0
            elif 'dropbyte' in a.fault and at >= 0x50000 and once('dropbyte'): enc = enc[:1000] + enc[1001:]
            elif 'droppacket' in a.fault and at >= 0x60000 and once('droppacket'): enc = b''
            w(enc); sent += len(chunk); stats['reads'] += 1
            while True:
                ack = read_frame(30)                     # the real stub waits without a timeout
                if ack is None: L('no ack'); return
                if 'dropack' in a.fault and off + sent >= 0x70000 and len(ack) == 4 and once('dropack'): continue
                break
            if len(ack) != 4: L('short ack: abort at', sent); break      # esp-flasher-stub: ends the loop
            got = struct.unpack('<I', ack)[0]
            if got != sent: L('ack mismatch', got, sent); return
        slip(h.digest())                                 # MD5 of what was sent (also after an abort)
    else:
        resp(op, 0, b'', 0xC300)

try:
    while True:
        rl = [master] + ([ctl] if ctl else [])
        r, _, _ = select.select(rl, [], [], 0.2)
        if ctl in r: handle_ctl()
        if master in r:
            try: data = os.read(master, 65536)
            except OSError: break
            if mode in ('rom', 'stub'): feed(data)
        while frames:
            handle(frames.pop(0))
        if a.flash:
            pass
except KeyboardInterrupt:
    pass
finally:
    open(a.flash + '.out', 'wb').write(flash)
    L('stats', stats)
