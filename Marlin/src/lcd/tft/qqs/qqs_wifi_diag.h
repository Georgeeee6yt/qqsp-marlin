/**
 * QQS-Pro: MKS Wi-Fi diagnostics, filled in by module/mks_wifi and shown on the info page's Wi-Fi tab.
 */
#pragma once
#include <stdint.h>

struct qqs_wifi_diag_t {
  // ESP -> printer (normal 115200 protocol)
  uint32_t rx_pkt[5];        // by type: 0 net, 1 G-code, 2 file first, 3 fragment, 4 wifi list
  uint32_t rx_other, rx_bad, rx_stray;   // unknown type / failed check / bytes outside a packet
  char last_in[3][28];       // last G-code lines from Wi-Fi (newest at last_in_i)
  uint8_t last_in_i;
  // printer -> ESP
  uint32_t tx_lines, tx_timeouts;
  char last_out[28];
  // last upload (high-speed DMA transfer)
  uint8_t up_count;
  char up_name[24];
  uint32_t up_size, up_recv, up_written, up_pk;
  char up_reason;            // 'O' ok, 'T' timeout (nothing / no more data), 'H' bad head, 'S' sector skip,
                             // 'W' SD write error, 'E' DMA error, 'Z' finished but sizes differ, 'M' mount, 'F' open
  uint16_t up_cndtr;         // DMA bytes still expected in the current 1 KB packet when it ended (1024 = nothing came)
  uint16_t up_sr;            // USART1 status at the end (FE 0x02 / NE 0x04 / ORE 0x08 = baud or noise trouble)
  uint8_t up_first[8];       // first 8 bytes of the first packet (or of the partial buffer if none completed)
  uint8_t up_io4;            // flow-control pin level at the end
  uint32_t up_last_sector;
  uint32_t up_end_ms;        // millis() when the last upload ended (any way), 0 = none yet
};

extern qqs_wifi_diag_t qqs_wdiag;

// Network state as the ESP reports it (type 0 packet, every 10 s; every 0.5 s while it tries to connect)
struct qqs_wifi_net_t {
  uint8_t mode;              // 1 AP (its own hotspot), 2 STA (joins a network), 0 nothing heard yet
  uint8_t state;             // raw: 0x0A up / connected, 0x0E exception, other = not connected
  char ssid[33];             // STA: network it joins; AP: its own hotspot name
  char key[65];              // password it holds (plain text, as the ESP reports it)
  uint16_t reports;          // type 0 packets parsed (wraps)
};
extern qqs_wifi_net_t qqs_wnet;

// Last scan result (type 4 packet, answer to type 7). The ESP drops networks weaker than -78 dBm.
#define QQS_WSCAN_MAX 16
struct qqs_wifi_scan_t {
  uint8_t n;                 // networks below, strongest first, duplicates merged
  uint16_t seq;              // bumps on every list received
  int8_t rssi[QQS_WSCAN_MAX];
  char name[QQS_WSCAN_MAX][33];
};
extern qqs_wifi_scan_t qqs_wscan;

// Printer -> ESP. The ESP saves new settings in its own flash and restarts by itself when they differ.
void qqs_wifi_config(const uint8_t mode, const char *ssid, const char *key);   // type 0 (+ type 9 "connect" for STA)
void qqs_wifi_scan();                                                           // type 7 (in AP mode this drops the hotspot)
void qqs_wifi_reset();                                                          // pulse the module reset: back to its saved settings
