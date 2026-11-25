#!/usr/bin/env python3
"""
Turn esptool's ESP8266 flasher stub (JSON) into ESPSTUB.BIN for the QQS-Pro firmware (SD card root).

  python3 qqs_tools/make_espstub.py <esptool>/esptool/targets/stub_flasher/2/esp8266.json ESPSTUB.BIN

Layout (little-endian):
  0  'QSTB'
  4  entry          8  text_start   12 text_len
  16 data_start     20 data_len     24 source tag (stub generation: 1 legacy, 2 esp-flasher-stub)
  28 text bytes, then data bytes

Sources / licences:
  stub_flasher/2: esp-flasher-stub v2.0.0, Apache-2.0 OR MIT (github.com/espressif/esp-flasher-stub)
  stub_flasher/1: esptool-legacy-flasher-stub v1.11.1, GPL-2.0-or-later
"""
import base64, json, struct, sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    j = json.load(open(src))
    text = base64.b64decode(j['text'])
    data = base64.b64decode(j.get('data', ''))
    tag = 2 if '/2/' in src.replace('\\', '/') else 1
    if j.get('plugins'):
        pass  # plugins are optional extras; the base stub works without them
    hdr = b'QSTB' + struct.pack('<6I', j['entry'], j['text_start'], len(text),
                                j.get('data_start', 0), len(data), tag)
    open(dst, 'wb').write(hdr + text + data)
    print('%s: entry %#x, text %#x+%d, data %#x+%d, %d bytes' % (
        dst, j['entry'], j['text_start'], len(text), j.get('data_start', 0), len(data), 28 + len(text) + len(data)))


if __name__ == '__main__':
    main()
