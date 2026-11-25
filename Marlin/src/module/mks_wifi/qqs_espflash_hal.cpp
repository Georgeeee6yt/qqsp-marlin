/**
 * QQS-Pro: board side of the ESP8266 flasher (qqs_espflash.cpp) on the FLSUN HiSpeed board.
 * USART1 (MYSERIAL2), GPIO0 = MKS_WIFI_IO0 (PA8), reset = MKS_WIFI_IO_RST (PA5).
 * The SD card is used through FatFs like the FB4S upload (mks_wifi_sd_init / _deinit), with its FIL
 * and its 8 KB shared_mem buffer (never used at the same time as an upload).
 */
#include "../../inc/MarlinConfig.h"

#ifdef MKS_WIFI

#include "mks_wifi.h"
#include "mks_wifi_sd.h"
#include "qqs_espflash.h"
#include "../../libs/fatfs/ff.h"
#include "../../libs/fatfs/fatfs_shared.h"
#include "../shared_mem/shared_mem.h"
#include "../../lcd/tft/qqs/qqs_wifi_diag.h"

static_assert(SHARED_MEM_SIZE >= 8192, "flasher needs 8 KB of shared_mem");
extern FIL upload_file;

uint8_t *espf_hal_buf() { return (uint8_t *)shared_mem; }
void espf_hal_uart_begin(const uint32_t baud) { MYSERIAL2.flush(); MYSERIAL2.end(); MYSERIAL2.begin(baud); }   // like M575
int  espf_hal_uart_read() { return MYSERIAL2.read(); }
void espf_hal_uart_write(const uint8_t c) { MYSERIAL2.write(c); }
void espf_hal_uart_drain() { MYSERIAL2.flush(); }

void espf_hal_pins(const bool boot_low, const bool reset_low) {
  OUT_WRITE(MKS_WIFI_IO0, !boot_low);
  OUT_WRITE(MKS_WIFI_IO_RST, !reset_low);
}

void espf_hal_release() {
  SET_INPUT(MKS_WIFI_IO0);           // as before: GPIO0 left to the module's pull-up
  espf_hal_uart_begin(BAUDRATE_2);
  mks_wifi_init();                   // the normal start: reset pulse, IO4 sequence
}

uint32_t espf_hal_ms() { return millis(); }
void espf_hal_keepalive() { hal.watchdog_refresh(); }

bool espf_hal_sd_open() {
  if (mks_wifi_sd_init() == FR_OK) return true;
  mks_wifi_sd_deinit();              // give the card back to Marlin
  return false;
}
void espf_hal_sd_close() { mks_wifi_sd_deinit(); }

static char path[32];
static const char *P(const char *n) { path[0] = '0'; path[1] = ':'; path[2] = '/'; strncpy(path + 3, n, sizeof(path) - 4); path[sizeof(path) - 1] = 0; return path; }

bool espf_hal_fsize(const char *name, uint32_t *size) {
  if (f_stat(P(name), (FILINFO *)&fno) != FR_OK) return false;
  if (size) *size = fno.fsize;
  return true;
}
bool espf_hal_fopen_read(const char *name)  { return f_open(&upload_file, P(name), FA_READ) == FR_OK; }
bool espf_hal_fopen_write(const char *name) { return f_open(&upload_file, P(name), FA_CREATE_NEW | FA_WRITE) == FR_OK; }
int espf_hal_fread(uint8_t *p, const uint16_t n) { UINT br; return f_read(&upload_file, p, n, &br) == FR_OK ? int(br) : -1; }
bool espf_hal_fwrite(const uint8_t *p, const uint16_t n) { UINT bw; return f_write(&upload_file, p, n, &bw) == FR_OK && bw == n; }
bool espf_hal_ftrunc(const uint32_t size) { return f_lseek(&upload_file, size) == FR_OK && f_truncate(&upload_file) == FR_OK; }
void espf_hal_fclose() { f_close(&upload_file); }
bool espf_hal_rename(const char *from, const char *to) {
  char a[32]; strcpy(a, P(from));
  f_unlink(P(to));                   // the MKS firmware keeps one MKS_WIFI_CUR
  return f_rename(a, P(to)) == FR_OK;
}
uint16_t espf_hal_net_reports() { return qqs_wnet.reports; }

// For the module-firmware page: which files are on the card (one mount, ~0.2 s)
void espf_scan_sd(uint32_t sizes[5]) {
  static const char * const names[5] = { "ESPSTUB.BIN", "ESPBAK.BIN", "MksWifi.bin", "MksWifi_WebView.bin", "MKS_WIFI_CUR" };
  for (uint8_t i = 0; i < 5; i++) sizes[i] = 0;
  if (!espf_hal_sd_open()) { sizes[0] = 0xFFFFFFFF; return; }   // marks "no card"
  for (uint8_t i = 0; i < 5; i++) if (!espf_hal_fsize(names[i], &sizes[i])) sizes[i] = 0;
  espf_hal_sd_close();
}

#endif // MKS_WIFI
