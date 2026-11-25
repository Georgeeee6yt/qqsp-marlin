// PC test harness for Marlin/src/module/mks_wifi/qqs_espflash.cpp: the same core, a host HAL.
//   host_test <pty> <ctl-fifo> <sd-dir> <op: backup|restore|fw|web>
// "SD card" = a directory (case-insensitive names like FatFs). "Pins" go to the fake ESP's control FIFO.
#include "../../Marlin/src/module/mks_wifi/qqs_espflash.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>

static int fd = -1, ctl = -1;
static std::string sd;
static uint8_t buf8k[8192];
static FILE *f = nullptr;
static bool boot_low = false, reset_low = false;
static uint16_t reports = 0;
static uint32_t released_at = 0;
static uint32_t baud_now = 0;
static unsigned long tx_bytes = 0;

uint8_t *espf_hal_buf() { return buf8k; }
void espf_hal_uart_begin(const uint32_t baud) { baud_now = baud; }
int espf_hal_uart_read() { uint8_t c; return read(fd, &c, 1) == 1 ? c : -1; }
void espf_hal_uart_write(const uint8_t c) { while (write(fd, &c, 1) != 1) usleep(100); tx_bytes++; }
void espf_hal_uart_drain() { tcdrain(fd); }
static void ctl_send(const char *s) { if (write(ctl, s, strlen(s)) < 0) perror("ctl"); }
void espf_hal_pins(const bool b, const bool r) {
  // a reset ends when reset goes high: tell the fake which mode it boots into
  if (reset_low && !r) ctl_send(b ? "reset 1\n" : "reset 0\n");
  boot_low = b; reset_low = r;
}
uint32_t espf_hal_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
void espf_hal_release() { ctl_send("reset 0\n"); released_at = espf_hal_ms(); baud_now = 115200; }
void espf_hal_keepalive() {}
static int sd_opens = 0;
bool espf_hal_sd_open() { if (getenv("NOSD")) return false; sd_opens++; return true; }   // failure leaves the card as it was
void espf_hal_sd_close() { sd_opens--; }

static std::string find(const char *name) {        // case-insensitive lookup in the SD dir
  DIR *d = opendir(sd.c_str()); if (!d) return "";
  std::string r;
  while (dirent *e = readdir(d)) if (!strcasecmp(e->d_name, name)) { r = sd + "/" + e->d_name; break; }
  closedir(d); return r;
}
bool espf_hal_fsize(const char *name, uint32_t *size) {
  std::string p = find(name); struct stat s;
  if (p.empty() || stat(p.c_str(), &s)) return false;
  if (size) *size = s.st_size;
  return true;
}
bool espf_hal_fopen_read(const char *name) { if (f) abort(); std::string p = find(name); f = p.empty() ? nullptr : fopen(p.c_str(), "rb"); return f; }
bool espf_hal_fopen_write(const char *name) { if (f) abort(); if (!find(name).empty()) return false; f = fopen((sd + "/" + name).c_str(), "wb"); return f; }
int espf_hal_fread(uint8_t *p, const uint16_t n) { return f ? (int)fread(p, 1, n, f) : -1; }
bool espf_hal_fwrite(const uint8_t *p, const uint16_t n) { return f && fwrite(p, 1, n, f) == n; }
void espf_hal_fclose() { if (f) fclose(f); f = nullptr; }
bool espf_hal_rename(const char *from, const char *to) {
  std::string a = find(from), b = find(to); if (a.empty()) return false;
  if (!b.empty()) unlink(b.c_str());
  return rename(a.c_str(), (sd + "/" + to).c_str()) == 0;
}
uint16_t espf_hal_net_reports() {   // the fake boots its "app" 0.3 s after reset 0 and sends a status packet
  if (released_at && espf_hal_ms() - released_at > 600) { reports++; released_at = 0; }
  return reports;
}

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage: host_test pty ctl sddir op\n"); return 2; }
  fd = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK);
  termios t; tcgetattr(fd, &t); cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t);
  ctl = open(argv[2], O_WRONLY);
  sd = argv[3];
  const std::string op = argv[4];
  espf_start(op == "backup" ? ESPF_BACKUP : op == "restore" ? ESPF_RESTORE : op == "fw" ? ESPF_FW : ESPF_WEB);
  static const char *stages[] = { "enter", "stub", "erase", "xfer", "verify", "reboot" };
  uint8_t last = 255; const uint32_t t0 = espf_hal_ms();
  while (espf_busy()) {
    espf_poll();
    if (espf.stage != last) { last = espf.stage; fprintf(stderr, "  [%5u ms] stage %s\n", espf_hal_ms() - t0, stages[last]); }
    usleep(200);                       // the rest of Marlin's idle loop
  }
  printf("RESULT op=%s err=%u code=%#x stage=%s baud=%u flash=%u id=%06x done=%u/%u bps=%u reported=%u file=%s sd_balanced=%d tx=%lu\n",
         op.c_str(), espf.err, espf.err_code, stages[espf.stage], espf.baud, espf.flash_size, espf.flash_id,
         espf.done, espf.total, espf.bps, espf.reported, espf.file, sd_opens == 0, tx_bytes);
  uint8_t m[16]; espf_md5((const uint8_t *)"abc", 3, m);
  printf("md5(abc)=");
  for (int i = 0; i < 16; i++) printf("%02x", m[i]);
  printf("\n");
  return espf.err ? 1 : 0;
}
