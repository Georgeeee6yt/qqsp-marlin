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
};

extern qqs_wifi_diag_t qqs_wdiag;
