/**
 * QQS-Pro: ESP8266 (MKS Wi-Fi module) backup / flashing over USART1. See qqs_espflash.h.
 *
 * Protocol (espressif/esptool 5.7 loader.py, esp-flasher-stub 2.0.0 command_handler.c):
 *  - SLIP frames (C0 ... C0, DB DC = C0, DB DD = DB).
 *  - Command: 00 op len16 chk32 data. Response: 01 op len16 val32 data status[2] (status[0] != 0 = error).
 *  - chk = XOR of the payload bytes starting from 0xEF (MEM_DATA / FLASH_DATA only).
 *  - SYNC 08: 07 07 12 20 + 32 x 55; the ROM answers several times.
 *  - Stub upload: MEM_BEGIN 05 (size, blocks, blocksize, addr), MEM_DATA 07 (len, seq, 0, 0 + data),
 *    MEM_END 06 (0, entry); the stub then sends the frame "OHAI".
 *  - Stub: CHANGE_BAUDRATE 0F (new, old), SPI_SET_PARAMS 0B (id, size, 64K, 4K, 256, 0xFFFF),
 *    ERASE_REGION D1 (addr, size), FLASH_BEGIN 02 (size, blocks, blocksize, addr, 0), FLASH_DATA 03,
 *    FLASH_END 04 (1 = stay), SPI_FLASH_MD5 13 (addr, size, 0, 0) -> 16 raw bytes,
 *    READ_FLASH D2 (addr, size, packet, inflight): the stub sends data frames, waits for an ACK frame
 *    holding the total received (le32) after each (one in flight), then sends the 16-byte MD5.
 *    It waits for the ACK without a timeout; an ACK frame that is not 4 bytes long ends the loop early
 *    (it still sends the MD5 of what it sent) and the stub is back to taking commands. We use that to
 *    resume after a lost / damaged packet: abort, drop what is in the pipe, READ_FLASH from where we are.
 *  - Flash ID: SPI "user command" 0x9F through READ_REG 0A / WRITE_REG 09 on the ESP8266 SPI block
 *    (esptool run_spiflash_command); size = 1 << (id >> 16).
 *  - esp-flasher-stub writes after answering FLASH_DATA: a write error shows up on the NEXT command,
 *    so FLASH_END is checked and the whole region is verified with MD5.
 */
#include "qqs_espflash.h"
#include <string.h>

EspfStatus espf;

// =====================================================================================
// MD5 (RFC 1321)
// =====================================================================================

struct Md5 { uint32_t h[4], n; uint8_t b[64]; };
static const uint32_t md5_k[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};
static const uint8_t md5_s[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void md5_block(uint32_t *h, const uint8_t *p) {
  uint32_t w[16];
  for (uint8_t i = 0; i < 16; i++) w[i] = p[i * 4] | (uint32_t(p[i * 4 + 1]) << 8) | (uint32_t(p[i * 4 + 2]) << 16) | (uint32_t(p[i * 4 + 3]) << 24);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  for (uint8_t i = 0; i < 64; i++) {
    uint32_t f; uint8_t g;
    switch (i >> 4) {
      case 0:  f = (b & c) | (~b & d); g = i; break;
      case 1:  f = (d & b) | (~d & c); g = (5 * i + 1) & 15; break;
      case 2:  f = b ^ c ^ d;          g = (3 * i + 5) & 15; break;
      default: f = c ^ (b | ~d);       g = (7 * i) & 15; break;
    }
    const uint32_t x = a + f + md5_k[i] + w[g];
    const uint8_t s = md5_s[((i >> 4) << 2) | (i & 3)];
    a = d; d = c; c = b;
    b += (x << s) | (x >> (32 - s));
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}
static void md5_init(Md5 &m) { m.h[0] = 0x67452301; m.h[1] = 0xefcdab89; m.h[2] = 0x98badcfe; m.h[3] = 0x10325476; m.n = 0; }
static void md5_update(Md5 &m, const uint8_t *p, uint32_t n) {
  uint32_t used = m.n & 63;
  m.n += n;
  while (n) {
    uint32_t k = 64 - used; if (k > n) k = n;
    memcpy(m.b + used, p, k); used += k; p += k; n -= k;
    if (used == 64) { md5_block(m.h, m.b); used = 0; }
  }
}
static void md5_final(Md5 &m, uint8_t *out) {
  const uint32_t bits_lo = m.n << 3, bits_hi = m.n >> 29;
  static const uint8_t pad80 = 0x80, zero = 0;
  md5_update(m, &pad80, 1);
  while ((m.n & 63) != 56) md5_update(m, &zero, 1);
  uint8_t l[8];
  for (uint8_t i = 0; i < 4; i++) { l[i] = bits_lo >> (8 * i); l[4 + i] = bits_hi >> (8 * i); }
  md5_update(m, l, 8);
  for (uint8_t i = 0; i < 16; i++) out[i] = m.h[i >> 2] >> (8 * (i & 3));
}
void espf_md5(const uint8_t *data, uint32_t len, uint8_t out[16]) { Md5 m; md5_init(m); md5_update(m, data, len); md5_final(m, out); }

// =====================================================================================
// SLIP link
// =====================================================================================

#define FBUF       (espf_hal_buf())          // 0..4095: file data / outgoing payload
#define RXB        (espf_hal_buf() + 4096)   // 4096..8191: incoming frame
#define RX_CAP     4096

static uint16_t rx_len = 0, rx_flen = 0;
static bool rx_in = false, rx_esc = false, rx_over = false;

static inline uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
static inline void put32(uint8_t *p, const uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void rx_reset() { rx_in = rx_esc = rx_over = false; rx_len = 0; while (espf_hal_uart_read() >= 0) {} }

// Take bytes from the UART; true when a whole frame is in RXB (length rx_flen).
static bool rx_pump() {
  int c;
  while ((c = espf_hal_uart_read()) >= 0) {
    if (c == 0xC0) {
      if (rx_in && rx_len && !rx_over) { rx_flen = rx_len; rx_len = 0; rx_esc = false; return true; }
      rx_in = true; rx_len = 0; rx_esc = rx_over = false;    // start (or empty / broken frame)
      continue;
    }
    if (!rx_in) continue;                                     // noise between frames (ROM boot text)
    if (rx_esc) { rx_esc = false; if (c == 0xDC) c = 0xC0; else if (c == 0xDD) c = 0xDB; }
    else if (c == 0xDB) { rx_esc = true; continue; }
    if (rx_len < RX_CAP) RXB[rx_len++] = c; else rx_over = true;
  }
  return false;
}

// Block until a frame arrives; length or -1 on timeout.
static int16_t rx_frame(const uint32_t timeout) {
  const uint32_t t0 = espf_hal_ms();
  for (;;) {
    if (rx_pump()) return rx_flen;
    if (espf_hal_ms() - t0 >= timeout) return -1;
    espf_hal_keepalive();
  }
}

static void tx(const uint8_t *p, uint16_t n) {
  while (n--) {
    const uint8_t c = *p++;
    if (c == 0xC0)      { espf_hal_uart_write(0xDB); espf_hal_uart_write(0xDC); }
    else if (c == 0xDB) { espf_hal_uart_write(0xDB); espf_hal_uart_write(0xDD); }
    else espf_hal_uart_write(c);
  }
}

static void send_cmd(const uint8_t op, const uint8_t *a, const uint16_t an, const uint8_t *b = nullptr, const uint16_t bn = 0, const uint32_t chk = 0) {
  uint8_t h[8] = { 0, op, uint8_t(an + bn), uint8_t((an + bn) >> 8) };
  put32(h + 4, chk);
  espf_hal_uart_write(0xC0);
  tx(h, 8); tx(a, an); if (bn) tx(b, bn);
  espf_hal_uart_write(0xC0);
}

static void send_ack(const uint32_t v) {      // READ_FLASH flow control: a bare frame with the total
  uint8_t p[4]; put32(p, v);
  espf_hal_uart_write(0xC0); tx(p, 4); espf_hal_uart_write(0xC0);
}

static uint8_t st;                            // internal state (below)
static void fail(const uint8_t e, const uint16_t code = 0);

// Look for the answer to `op`. 1 = ok (data in resp_data / resp_val), 0 = not yet, -1 = failed.
static const uint8_t *resp_data;
static uint32_t resp_val;
// soft: an error status is not fail()ed but returned as -2 with the code in *soft (caller decides).
static int8_t resp_poll(const uint8_t op, const uint8_t resp_len = 0, uint16_t *soft = nullptr) {
  while (rx_pump()) {
    const uint8_t *p = RXB;
    if (rx_flen < 8 || p[0] != 1 || p[1] != op) continue;   // not ours (extra SYNC answers, noise)
    resp_val = le32(p + 4);
    const uint8_t *d = p + 8;
    const uint16_t dl = rx_flen - 8;
    const uint8_t *e = (dl >= resp_len + 2u) ? d + resp_len : (dl >= 2 ? d : nullptr);
    if (e && e[0] && soft) { *soft = (e[0] << 8) | e[1]; return -2; }
    if (dl < resp_len + 2u) { if (dl >= 2 && d[0]) fail(EE_CMD, (d[0] << 8) | d[1]); else fail(EE_DATA); return -1; }
    if (d[resp_len]) { fail(EE_CMD, (d[resp_len] << 8) | d[resp_len + 1]); return -1; }
    resp_data = d;
    return 1;
  }
  return 0;
}

// Send and wait (blocking). false = failed (fail() done unless quiet).
static bool cmd(const uint8_t op, const uint8_t *a, const uint16_t an, const uint32_t timeout = 3000,
                const uint8_t resp_len = 0, const uint8_t *b = nullptr, const uint16_t bn = 0, const uint32_t chk = 0, const bool quiet = false) {
  send_cmd(op, a, an, b, bn, chk);
  const uint32_t t0 = espf_hal_ms();
  for (;;) {
    const int8_t r = resp_poll(op, resp_len);
    if (r) return r > 0;
    if (espf_hal_ms() - t0 >= timeout) { if (!quiet) fail(EE_TIMEOUT); return false; }
    espf_hal_keepalive();
  }
}

static bool cmd4(const uint8_t op, const uint32_t a, const uint32_t b, const uint32_t c, const uint32_t d, const uint32_t timeout = 3000) {
  uint8_t p[16]; put32(p, a); put32(p + 4, b); put32(p + 8, c); put32(p + 12, d);
  return cmd(op, p, 16, timeout);
}

static bool read_reg(const uint32_t addr, uint32_t &v, const bool quiet = false) {
  uint8_t p[4]; put32(p, addr);
  if (!cmd(0x0A, p, 4, quiet ? 500 : 3000, 0, nullptr, 0, 0, quiet)) return false;
  v = resp_val;
  return true;
}
static bool write_reg(const uint32_t addr, const uint32_t v) { return cmd4(0x09, addr, v, 0xFFFFFFFF, 0); }

// =====================================================================================
// State machine
// =====================================================================================

enum : uint8_t {
  S_IDLE, S_OPEN, S_RESET, S_RESET2, S_SYNC, S_STUB, S_OHAI, S_BAUD, S_BAUD2, S_FLASHID,
  S_ERASE, S_ERASE_WAIT, S_WRITE_BEGIN, S_WRITE, S_MD5, S_MD5_WAIT, S_READ_BEGIN, S_READ_CMD, S_READ,
  S_READ_ABORT, S_BAKCHK,
  S_REBOOT, S_REBOOT_WAIT, S_DONE
};

static const uint32_t bauds[] = { 921600, 460800, 115200 };
static uint8_t baud_i, tries, sync_n;
static uint32_t t_ms, t_xfer, tmo;
static uint32_t addr, img_size, pos, seq;
static uint16_t blk;
static uint8_t wr_h[16], wr_chk;               // the FLASH_DATA block in flight (for a resend)
static bool owns = false, sd_on = false, f_on = false, waiting = false;
static uint16_t rep0;
static Md5 md;
static Md5 md_seg;                   // backup: the current READ_FLASH (resumes start a new one)
static uint32_t seg0, seg_end;        // backup: the current READ_FLASH (at most READ_SEG bytes)
#define READ_SEG 0x40000              // 256 KB per READ_FLASH: a damaged packet costs at most this again
#define READ_RESUMES 8
#define WRITE_RESENDS 16                      // FLASH_DATA blocks sent again after the stub saw them damaged
static const char *img_name;
static uint8_t md5_ours[16];

#define STUB_NAME "ESPSTUB.BIN"
#define BAK_NAME  "ESPBAK.BIN"

bool espf_busy() { return espf.running; }
bool espf_owns_uart() { return owns; }

static void set_stage(const uint8_t s) { espf.stage = s; espf.stage_ms = espf_hal_ms(); }
static void go(const uint8_t s) { st = s; t_ms = espf_hal_ms(); tries = 0; }

static void close_file() { if (f_on) { espf_hal_fclose(); f_on = false; } }
static void close_sd() { close_file(); if (sd_on) { espf_hal_sd_close(); sd_on = false; } }

static void fail(const uint8_t e, const uint16_t code) {
  if (espf.err) return;               // keep the first reason
  espf.err = e;
  espf.err_code = code;
  close_sd();
  if (owns) { owns = false; espf_hal_release(); }   // the module boots whatever is in its flash
  espf.running = 0;
  st = S_IDLE;
}

void espf_start(const EspfOp op) {
  if (espf.running) return;
  memset(&espf, 0, sizeof(espf));
  espf.op = op;
  espf.running = 1;
  set_stage(EST_ENTER);
  go(S_OPEN);
}

static uint32_t timeout_mb(const uint32_t per_mb, const uint32_t size) {
  const uint32_t t = uint32_t((uint64_t(per_mb) * size) >> 20);
  return t < 3000 ? 3000 : t;
}

static void xfer_progress() {
  espf.done = pos;
  const uint32_t el = espf_hal_ms() - t_xfer;
  if (el > 300) espf.bps = uint32_t(uint64_t(pos) * 1000 / el);
}

// Open the image for writing to the ESP; false = failed (reason set)
static bool open_image() {
  if (!espf_hal_fopen_read(img_name)) { fail(EE_SDWRITE); return false; }
  f_on = true;
  return true;
}

// Backup: a packet went missing / came short / the part's MD5 differs. End the stub's READ_FLASH loop
// with a non-4-byte "ACK" and start a new READ_FLASH at pos (abort = false: the stub already finished).
static void read_resume(const uint8_t why, const bool abort = true) {
  if (++espf.retries > READ_RESUMES) return fail(why);
  if (abort) { espf_hal_uart_write(0xC0); espf_hal_uart_write(0x00); espf_hal_uart_write(0xC0); }
  rx_in = rx_esc = rx_over = false; rx_len = 0;
  espf.done = pos;
  go(S_READ_ABORT);
}

void espf_poll() {
  if (!espf.running) return;
  const uint32_t now = espf_hal_ms();

  switch (st) {

    // ---- check the card, files and sizes before touching the module ----
    case S_OPEN: {
      if (!espf_hal_sd_open()) return fail(EE_SD);
      sd_on = true;
      uint32_t sz = 0;
      if (!espf_hal_fsize(STUB_NAME, &sz) || sz < 28) return fail(EE_NOSTUB);
      switch (espf.op) {
        case ESPF_BACKUP: {
          addr = 0;
          strcpy(espf.file, BAK_NAME);
          for (uint8_t i = 1; espf_hal_fsize(espf.file, nullptr); i++) {
            if (i > 9) return fail(EE_FULL);
            espf.file[6] = '0' + i; strcpy(espf.file + 7, ".BIN");
          }
          break;
        }
        case ESPF_RESTORE: img_name = BAK_NAME; addr = 0; break;
        case ESPF_FW:      img_name = "MksWifi.bin"; addr = 0; break;
        default:           img_name = "MksWifi_WebView.bin"; addr = 0x100000; break;
      }
      if (espf.op != ESPF_BACKUP) {
        if (!espf_hal_fsize(img_name, &img_size)) return fail(EE_NOFILE);
        const uint32_t room = espf.op == ESPF_FW ? 0x100000 : espf.op == ESPF_WEB ? 0x2FB000 : 0x1000000;
        if (!img_size || img_size > room) return fail(EE_SIZE);
        if (espf.op == ESPF_FW) {        // an ESP8266 app image starts with 0xE9
          if (!open_image()) return;
          const bool ok = espf_hal_fread(FBUF, 1) == 1 && FBUF[0] == 0xE9;
          close_file();
          if (!ok) return fail(EE_NOTIMG);
        }
      }
      owns = true;                       // from here mks_wifi keeps off USART1
      baud_i = sync_n = 0;
      espf_hal_uart_begin(115200);
      go(S_RESET);
      espf.done = 0;
    } __attribute__((fallthrough));
    case S_RESET:                        // GPIO0 low + reset low (esptool: 100 ms)
      espf_hal_pins(true, true);
      st = S_RESET2; t_ms = now;
      return;

    case S_RESET2:
      if (now - t_ms < 100) return;
      espf_hal_pins(true, false);        // out of reset with GPIO0 low: ROM download mode
      st = S_SYNC; t_ms = espf_hal_ms();
      return;

    case S_SYNC: {                       // one attempt per call (100 ms), 6 per reset, 3 resets
      if (now - t_ms < 60) return;       // let the ROM boot text pass
      rx_reset();
      uint8_t p[36] = { 0x07, 0x07, 0x12, 0x20 };
      memset(p + 4, 0x55, 32);
      if (cmd(0x08, p, 36, 100, 0, nullptr, 0, 0, true)) {
        const uint32_t t0 = espf_hal_ms();          // swallow the extra SYNC answers
        while (espf_hal_ms() - t0 < 100) { rx_pump(); espf_hal_keepalive(); }
        rx_reset();
        set_stage(EST_STUB);
        go(S_STUB);
        return;
      }
      if (espf.err) return;
      if (++sync_n % 6 == 0) {
        if (sync_n >= 18) return fail(EE_SYNC);
        espf_hal_pins(true, true); st = S_RESET2; t_ms = espf_hal_ms();
      }
      return;
    }

    // ---- flasher stub from the SD card: header 'QSTB' entry text_start text_len data_start data_len tag ----
    case S_STUB: {
      if (!espf_hal_fopen_read(STUB_NAME)) return fail(EE_NOSTUB);
      f_on = true;
      uint8_t *h = FBUF;
      if (espf_hal_fread(h, 28) != 28 || memcmp(h, "QSTB", 4)) return fail(EE_NOSTUB);
      const uint32_t entry = le32(h + 4);
      uint32_t seg_addr[2] = { le32(h + 8), le32(h + 16) }, seg_len[2] = { le32(h + 12), le32(h + 20) };
      if (!seg_len[0] || seg_len[0] > 0x10000 || seg_len[1] > 0x8000) return fail(EE_NOSTUB);
      for (uint8_t s = 0; s < 2; s++) {
        if (!seg_len[s]) continue;
        const uint32_t blocks = (seg_len[s] + 1023) / 1024;
        if (!cmd4(0x05, seg_len[s], blocks, 1024, seg_addr[s])) return;
        for (uint32_t q = 0; q < blocks; q++) {
          const uint16_t n = (seg_len[s] - q * 1024) > 1024 ? 1024 : uint16_t(seg_len[s] - q * 1024);
          uint8_t *d = FBUF + 16;
          if (espf_hal_fread(d, n) != n) return fail(EE_NOSTUB);
          uint8_t chk = 0xEF;
          for (uint16_t i = 0; i < n; i++) chk ^= d[i];
          put32(FBUF, n); put32(FBUF + 4, q); put32(FBUF + 8, 0); put32(FBUF + 12, 0);
          if (!cmd(0x07, FBUF, 16, 3000, 0, d, n, chk)) return;
          espf_hal_keepalive();
        }
      }
      close_file();
      uint8_t e[8]; put32(e, 0); put32(e + 4, entry);
      send_cmd(0x06, e, 8);              // the ROM may or may not answer; the stub says OHAI
      go(S_OHAI);
      return;
    }

    case S_OHAI:
      while (rx_pump()) if (rx_flen == 4 && !memcmp(RXB, "OHAI", 4)) { go(S_BAUD); return; }
      if (now - t_ms > 3000) return fail(EE_OHAI);
      return;

    case S_BAUD: {
      const uint32_t b = bauds[baud_i];
      if (b != 115200) {
        uint8_t p[8]; put32(p, b); put32(p + 4, 115200);
        if (!cmd(0x0F, p, 8)) return;
        espf_hal_uart_drain();
        espf_hal_uart_begin(b);
      }
      st = S_BAUD2; t_ms = espf_hal_ms();
      return;
    }

    case S_BAUD2: {                      // does the new speed work? (read the MAC word)
      if (now - t_ms < 50) return;
      rx_reset();
      uint32_t v;
      if (read_reg(0x3FF00050, v, true)) {
        espf.baud = bauds[baud_i];
        blk = espf.baud >= 460800 ? 4096 : 1024;
        go(S_FLASHID);
        return;
      }
      if (espf.err) return;
      if (++baud_i >= sizeof(bauds) / sizeof(bauds[0])) return fail(EE_BAUD);
      espf_hal_uart_begin(115200);       // start over (reset kills the stub) at the next speed
      set_stage(EST_ENTER);
      sync_n = 0;
      go(S_RESET);
      return;
    }

    case S_FLASHID: {                    // esptool run_spiflash_command(0x9F, read 24 bits) on the ESP8266
      const uint32_t base = 0x60000200, CMD = base, USR = base + 0x1C, USR1 = base + 0x20, USR2 = base + 0x24, W0 = base + 0x40;
      uint32_t old_usr, old_usr2, v = 0, id = 0;
      if (!read_reg(USR, old_usr) || !read_reg(USR2, old_usr2)) return;
      if (!write_reg(USR1, 23UL << 8) || !write_reg(USR, (1UL << 31) | (1UL << 28))
          || !write_reg(USR2, (7UL << 28) | 0x9F) || !write_reg(W0, 0) || !write_reg(CMD, 1UL << 18)) return;
      uint8_t i = 0;
      for (; i < 10; i++) { if (!read_reg(CMD, v)) return; if (!(v & (1UL << 18))) break; }
      if (i == 10) return fail(EE_FLASHID);
      if (!read_reg(W0, id) || !write_reg(USR, old_usr) || !write_reg(USR2, old_usr2)) return;
      espf.flash_id = id & 0xFFFFFF;
      const uint8_t size_id = (id >> 16) & 0xFF;
      if (size_id < 0x12 || size_id > 0x18) return fail(EE_FLASHID);
      espf.flash_size = 1UL << size_id;
      uint8_t p[24]; put32(p, 0); put32(p + 4, espf.flash_size); put32(p + 8, 0x10000); put32(p + 12, 0x1000); put32(p + 16, 0x100); put32(p + 20, 0xFFFF);
      if (!cmd(0x0B, p, 24)) return;

      if (espf.op == ESPF_BACKUP) { set_stage(EST_XFER); go(S_READ_BEGIN); return; }
      if (addr + img_size > espf.flash_size) return fail(EE_SIZE);
      set_stage(EST_ERASE);
      go(S_ERASE);
      return;
    }

    // ---- write: erase, then blocks, then MD5 ----
    case S_ERASE: {
      const uint32_t len = (img_size + 0xFFF) & ~0xFFFUL;
      uint8_t p[8]; put32(p, addr); put32(p + 4, len);
      send_cmd(0xD1, p, 8);
      tmo = timeout_mb(40000, len);       // esptool: 40 s per MB
      go(S_ERASE_WAIT);
      return;
    }

    case S_ERASE_WAIT: {
      const int8_t r = resp_poll(0xD1);
      if (r < 0) return;
      if (r > 0) { set_stage(EST_XFER); go(S_WRITE_BEGIN); return; }
      if (now - t_ms > tmo) return fail(EE_TIMEOUT);
      return;
    }

    case S_WRITE_BEGIN: {
      if (!open_image()) return;
      md5_init(md);
      pos = seq = 0;
      waiting = false;
      espf.total = img_size;
      uint8_t p[20]; put32(p, img_size); put32(p + 4, (img_size + blk - 1) / blk); put32(p + 8, blk); put32(p + 12, addr); put32(p + 16, 0);
      if (!cmd(0x02, p, 20, 10000)) return;
      t_xfer = espf_hal_ms();
      go(S_WRITE);
      return;
    }

    case S_WRITE: {
      if (waiting) {                      // answer to the last block
        uint16_t code = 0;
        const int8_t r = resp_poll(0x03, 0, &code);
        if (r == -2) {
          // The stub checks length and checksum BEFORE it writes and does not move on (esp-flasher-stub
          // 2.0.0 s_flash_data), so a block damaged on the wire can simply be sent again, same seq.
          const bool wire = code == 0xC100 || code == 0xC000 || code == 0xC800 || code == 0xC900;
          if (!wire || ++espf.retries > WRITE_RESENDS) return fail(EE_CMD, code);
          send_cmd(0x03, wr_h, 16, FBUF, blk, wr_chk);
          t_ms = espf_hal_ms();
          return;
        }
        if (r < 0) return;
        if (!r) { if (now - t_ms > 10000) fail(EE_TIMEOUT); return; }
        waiting = false;
        xfer_progress();
      }
      if (pos >= img_size) {
        close_file();
        uint8_t p[4]; put32(p, 1);         // stay in the stub; a pending write error is reported here
        if (!cmd(0x04, p, 4, 10000)) return;
        md5_final(md, md5_ours);
        set_stage(EST_VERIFY);
        go(S_MD5);
        return;
      }
      uint8_t *d = FBUF;
      const uint16_t n = (img_size - pos) > blk ? blk : uint16_t(img_size - pos);
      if (espf_hal_fread(d, n) != n) return fail(EE_SDWRITE);
      md5_update(md, d, n);
      if (n < blk) memset(d + n, 0xFF, blk - n);      // the stub only writes what is left of the size
      uint8_t chk = 0xEF;
      for (uint16_t i = 0; i < blk; i++) chk ^= d[i];
      put32(wr_h, blk); put32(wr_h + 4, seq); put32(wr_h + 8, 0); put32(wr_h + 12, 0);
      wr_chk = chk;                       // FBUF keeps the block until the stub has taken it
      send_cmd(0x03, wr_h, 16, d, blk, chk);
      pos += n; seq++;
      waiting = true; t_ms = espf_hal_ms();
      return;
    }

    case S_MD5: {
      const uint32_t len = img_size;
      uint8_t p[16]; put32(p, addr); put32(p + 4, len); put32(p + 8, 0); put32(p + 12, 0);
      send_cmd(0x13, p, 16);
      tmo = timeout_mb(8000, len);         // esptool: 8 s per MB
      go(S_MD5_WAIT);
      return;
    }

    case S_MD5_WAIT: {
      const int8_t r = resp_poll(0x13, 16);
      if (r < 0) return;
      if (!r) { if (now - t_ms > tmo) fail(EE_TIMEOUT); return; }
      if (memcmp(resp_data, md5_ours, 16)) return fail(EE_MD5);
      // verified: rename like the MKS firmware does, so the file is not flashed twice
      if (espf.op == ESPF_FW && !espf_hal_rename(img_name, "MKS_WIFI_CUR")) return fail(EE_RENAME);
      if (espf.op == ESPF_WEB && !espf_hal_rename(img_name, "MKS_WEB_CONTROL_CUR")) return fail(EE_RENAME);
      go(S_REBOOT);
      return;
    }

    // ---- backup: READ_FLASH, one packet per call; resumed where it stopped if a packet is lost ----
    case S_READ_BEGIN:
      if (!espf_hal_fopen_write(espf.file)) return fail(EE_SDWRITE);
      f_on = true;
      pos = 0;
      img_size = espf.total = espf.flash_size;
      t_xfer = espf_hal_ms();
      go(S_READ_CMD);
      return;

    case S_READ_CMD: {                      // READ_FLASH from pos to the end
      seg0 = pos;
      seg_end = (img_size - pos) > READ_SEG ? pos + READ_SEG : img_size;
      md5_init(md_seg);
      rx_reset();
      {
        uint8_t p[16]; put32(p, pos); put32(p + 4, seg_end - pos); put32(p + 8, blk); put32(p + 12, 1);
        if (!cmd(0xD2, p, 16, 3000, 0, nullptr, 0, 0, true)) {
          if (espf.err) return;               // the stub answered with an error status: fail() done
          return read_resume(EE_TIMEOUT, false);
        }
      }
      go(S_READ);
    } __attribute__((fallthrough));         // the first packet follows the answer at once
    case S_READ: {
      if (pos > seg0) send_ack(pos - seg0);   // the ESP sends the next packet only after this
      if (pos >= seg_end) {                // after the last ACK comes the MD5 of this READ_FLASH
        const int16_t n = rx_frame(1500);
        if (n != 16) return read_resume(n < 0 ? EE_TIMEOUT : EE_DATA);
        uint8_t m[16];
        md5_final(md_seg, m);
        if (memcmp(RXB, m, 16)) {           // a damaged byte with the right length: read this part again
          if (!espf_hal_ftrunc(seg0)) return fail(EE_SDWRITE);
          pos = seg0;
          return read_resume(EE_MD5, false);   // the stub is already back to commands
        }
        if (pos < img_size) { go(S_READ_CMD); return; }   // next segment
        close_file();
        set_stage(EST_VERIFY);
        if (!espf_hal_fopen_read(espf.file)) return fail(EE_SDWRITE);
        f_on = true;
        md5_init(md); pos = 0;
        go(S_BAKCHK);                       // file on the card, then its MD5 against the whole chip
        return;
      }
      // receive the whole packet now: the UART buffer is small and the ESP is already sending
      const int16_t n = rx_frame(1500);
      const uint32_t want = (seg_end - pos) > blk ? blk : seg_end - pos;
      if (n < 0 || uint32_t(n) != want) return read_resume(n < 0 ? EE_TIMEOUT : EE_DATA);
      md5_update(md_seg, RXB, n);
      if (!espf_hal_fwrite(RXB, n)) return fail(EE_SDWRITE);
      pos += n;
      xfer_progress();
      return;                               // ACK at the start of the next call
    }

    case S_READ_ABORT:                      // drop whatever the stub still sends (its MD5), then go on
      if (espf_hal_uart_read() >= 0) { t_ms = now; return; }
      if (now - t_ms < 300) return;
      go(S_READ_CMD);
      return;

    case S_BAKCHK: {                      // read the backup file back, 4 KB per call
      const uint16_t n = (img_size - pos) > 4096 ? 4096 : uint16_t(img_size - pos);
      if (n) {
        if (espf_hal_fread(FBUF, n) != n) return fail(EE_SDWRITE);
        md5_update(md, FBUF, n);
        pos += n;
        return;
      }
      md5_final(md, md5_ours);              // what is on the card ...
      close_file();
      addr = 0;                             // ... against the ESP's MD5 of the whole chip
      go(S_MD5);
      return;
    }

    // ---- give the module back and wait for its status packet ----
    case S_REBOOT:
      set_stage(EST_REBOOT);
      close_sd();
      owns = false;
      espf_hal_release();
      rep0 = espf_hal_net_reports();
      go(S_REBOOT_WAIT);
      return;

    case S_REBOOT_WAIT:
      if (espf_hal_net_reports() != rep0) espf.reported = 1;
      else if (now - t_ms < 40000) return;
      espf.running = 0;
      st = S_DONE;
      return;

    default: return;
  }
}
