/**
 * QQS-Pro: back up / flash the MKS Wi-Fi module (ESP8266) from the printer, no USB-serial adapter.
 *
 * The board pulls the module's GPIO0 low and pulses its reset: the ESP8266 ROM starts its serial
 * download mode on USART1. We then speak the esptool protocol (SLIP frames), load Espressif's flasher
 * stub from the SD card (ESPSTUB.BIN), switch to a high baud rate, and read / erase / write / MD5.
 * Protocol details follow espressif/esptool 5.7 (loader.py) and esp-flasher-stub 2.0.0.
 *
 * The core (qqs_espflash.cpp) is portable: everything board specific goes through the espf_hal_*
 * functions below (board: qqs_espflash_hal.cpp, PC test: qqs_tools/espflash_test/).
 * It is a state machine advanced by espf_poll() from the UI idle loop; each call blocks for at most
 * about one 4 KB packet (~45 ms at 921600 baud), long waits (erase, MD5) are polled.
 */
#pragma once
#include <stdint.h>

// What to do
enum EspfOp : uint8_t {
  ESPF_BACKUP = 0,     // whole flash -> ESPBAK.BIN (ESPBAK1..9.BIN if taken)
  ESPF_RESTORE,        // ESPBAK.BIN -> 0x0
  ESPF_FW,             // MksWifi.bin -> 0x0, then renamed MKS_WIFI_CUR
  ESPF_WEB             // MksWifi_WebView.bin -> 0x100000 (SPIFFS), then renamed MKS_WEB_CONTROL_CUR
};

// Stages shown to the user
enum EspfStage : uint8_t { EST_ENTER = 0, EST_STUB, EST_ERASE, EST_XFER, EST_VERIFY, EST_REBOOT, EST_COUNT };

// Why it stopped
enum EspfErr : uint8_t {
  EE_NONE = 0,
  EE_SD,          // SD card could not be mounted
  EE_NOSTUB,      // ESPSTUB.BIN missing or not a stub file
  EE_NOFILE,      // image file missing
  EE_SIZE,        // image too big for its place / empty / backup bigger than the chip
  EE_NOTIMG,      // MksWifi.bin does not start with 0xE9
  EE_FULL,        // ESPBAK.BIN .. ESPBAK9.BIN all taken
  EE_SYNC,        // no answer from the ROM (download mode not entered)
  EE_OHAI,        // stub did not start
  EE_BAUD,        // no baud rate worked after the stub
  EE_FLASHID,     // flash ID unreadable / unknown size
  EE_CMD,         // a command answered with an error status (code in err_code)
  EE_TIMEOUT,     // no answer in time
  EE_DATA,        // corrupt / short data from the ESP
  EE_MD5,         // MD5 mismatch
  EE_SDWRITE,     // SD read / write / create failed
  EE_RENAME       // written and verified, but the SD file could not be renamed
};

struct EspfStatus {
  uint8_t op;           // EspfOp
  uint8_t stage;        // EspfStage now (or where it failed)
  uint8_t running;      // 1 while busy
  uint8_t err;          // EspfErr, EE_NONE = ok so far
  uint16_t err_code;    // status code from the ESP for EE_CMD
  uint8_t reported;     // after the reboot: 1 = the module sent a status packet
  uint32_t done, total; // bytes for the transfer stage
  uint32_t bps;         // bytes / s in the transfer stage
  uint32_t baud;        // link speed reached
  uint32_t flash_size;  // from the flash ID
  uint32_t flash_id;    // raw RDID (manufacturer | device << 8 ...)
  uint32_t stage_ms;    // when the current stage started (espf_hal_ms)
  char file[13];        // 8.3 name of the backup file written
};
extern EspfStatus espf;

void espf_start(const EspfOp op);
void espf_poll();                 // call often (idle)
bool espf_busy();                 // running (any stage)
bool espf_owns_uart();            // USART1 belongs to the flasher: keep mks_wifi away
void espf_md5(const uint8_t *data, uint32_t len, uint8_t out[16]);   // one-shot (tests)

// ---- HAL (implemented per platform) ----
uint8_t *espf_hal_buf();                       // 8 KB scratch (board: FB4S shared_mem)
void espf_hal_uart_begin(const uint32_t baud);
int  espf_hal_uart_read();                     // -1 = nothing
void espf_hal_uart_write(const uint8_t c);
void espf_hal_uart_drain();                    // wait until sent
void espf_hal_pins(const bool boot_low, const bool reset_low);
void espf_hal_release();                       // flasher done: normal module start, normal serial
uint32_t espf_hal_ms();
void espf_hal_keepalive();                     // watchdog
bool espf_hal_sd_open();                       // mount the card for us (on failure: card left as it was)
void espf_hal_sd_close();                      // give it back to Marlin
// One file open at a time (board: FB4S's upload FIL). Names are long-name capable (FatFs LFN).
bool espf_hal_fsize(const char *name, uint32_t *size);   // exists? (size may be null)
bool espf_hal_fopen_read(const char *name);
bool espf_hal_fopen_write(const char *name);             // create new
int  espf_hal_fread(uint8_t *p, const uint16_t n);       // bytes read, -1 error
bool espf_hal_fwrite(const uint8_t *p, const uint16_t n);
void espf_hal_fclose();
bool espf_hal_rename(const char *from, const char *to);  // replaces "to"
uint16_t espf_hal_net_reports();               // status packets parsed by mks_wifi so far
