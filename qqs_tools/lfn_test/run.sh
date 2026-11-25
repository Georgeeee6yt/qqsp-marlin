#!/bin/bash
# Host test of the QQS long-name reader (CardReader::qqs_next) against a FAT16 image made by pyfatfs
# (pip install pyfatfs): CJK, emoji (surrogate pair), > 104 characters, a folder.
set -e
cd "$(dirname "$0")"; W=$(mktemp -d)
python3 - "$W" <<'PY'
import sys, struct
from pyfatfs.PyFatFS import PyFatFS
from pyfatfs.PyFat import PyFat
w = sys.argv[1]; img = w + '/sd.img'
open(img, 'wb').write(b'\0' * 8 * 1024 * 1024)
pf = PyFat(); pf.mkfs(img, fat_type=16, size=8 * 1024 * 1024, label='QQS'); pf.close()
f = PyFatFS(img)
for n in ['燈罩_v3.gcode', 'lampshade_QQSPro_157mm.gcode', 'TEST.GCO', 'readme.txt', 'emoji_😀_test.gcode', '長' * 120 + '.gcode']:
    with f.openbin('/' + n, 'w') as h: h.write(b'G28\n')
f.makedir('/模型資料夾')
with f.openbin('/模型資料夾/貓.gcode', 'w') as h: h.write(b'G1\n')
f.close()
d = open(img, 'rb').read()
bps, spc, res, nf, rootent, _, _, fatsz = struct.unpack_from('<HBHBHHBH', d, 11)
o = (res + nf * fatsz) * bps
open(w + '/root.bin', 'wb').write(d[o:o + rootent * 32])
PY
python3 -c "
s=open('../../Marlin/src/sd/cardreader.cpp').read()
open('$W/extract.inc','w').write(s[s.index('static uint8_t qqs_sfn_sum'):s.index('#endif // QQS_UI')])"
g++ -std=c++17 -Wall -fsanitize=address,undefined -I"$W" -o "$W/t" lfn_test.cpp
"$W/t" "$W/root.bin" 300 test.gco
echo "--- cap 40:"; "$W/t" "$W/root.bin" 40
