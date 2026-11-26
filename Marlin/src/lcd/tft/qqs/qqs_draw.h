/**
 * QQS-Pro v3 UI — drawing helpers on top of the Color UI canvas queue.
 */
#pragma once

#include "../../../inc/MarlinConfigPre.h"
#include "qqs_font.h"
#include "qqs_assets.h"

typedef void (*screenFunc_t)();

namespace qqs {

  // RGB565 palette
  #define QRGB(r,g,b) uint16_t((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
  constexpr uint16_t C_BG     = QRGB(0x1A, 0x1A, 0x1A);
  constexpr uint16_t C_BAR    = QRGB(0x10, 0x10, 0x10);
  constexpr uint16_t C_TILE   = QRGB(0x2A, 0x2A, 0x2A);
  constexpr uint16_t C_BORDER = QRGB(0x44, 0x44, 0x44);
  constexpr uint16_t C_TEXT   = QRGB(0xEA, 0xEA, 0xEA);
  constexpr uint16_t C_MUTED  = QRGB(0x9A, 0x9A, 0x9A);
  constexpr uint16_t C_DIM    = QRGB(0x5A, 0x5A, 0x5A);
  constexpr uint16_t C_ACCENT = QRGB(0xFF, 0x8C, 0x28);   // orange
  constexpr uint16_t C_HOT    = QRGB(0xF0, 0x6A, 0x3C);
  constexpr uint16_t C_COLD   = QRGB(0x5A, 0xA0, 0xE6);
  constexpr uint16_t C_OK     = QRGB(0x5D, 0xCA, 0xA5);
  constexpr uint16_t C_WARN   = QRGB(0xFA, 0xC7, 0x75);
  constexpr uint16_t C_STOP   = QRGB(0xE0, 0x4A, 0x40);
  constexpr uint16_t C_STOPBG = QRGB(0x4A, 0x1C, 0x18);

  #define F_S  qqs_font_s
  #define F_M  qqs_font_m
  #define F_L  qqs_font_l
  #define F_XL qqs_font_xl

  // Start a canvas (absolute screen coords) and clear it to bg.
  void canvas(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t bg);

  // Text / bitmaps inside the current canvas (coords relative to the canvas).
  void text(int16_t x, int16_t y, const qqs_font_t &f, uint16_t color, uint16_t bg, const char *s);
  void text_c(int16_t x, int16_t y, int16_t w, const qqs_font_t &f, uint16_t color, uint16_t bg, const char *s); // centred in [x, x+w)
  void text_r(int16_t xr, int16_t y, const qqs_font_t &f, uint16_t color, uint16_t bg, const char *s);           // right edge at xr
  inline uint16_t width(const qqs_font_t &f, const char *s) { return qqs_text_width(&f, s); }
  void bitmap(int16_t x, int16_t y, const qqs_bitmap_t &b, uint16_t color, uint16_t bg);
  void frame(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

  // A whole button: own canvas, border, optional icon above the label, touch area -> cb.
  struct Btn {
    const qqs_bitmap_t *icon = nullptr;
    const char *label = nullptr;
    const char *sub = nullptr;        // second, smaller line (S font)
    uint16_t fg = C_TEXT, bg = C_TILE, border = C_BORDER;
    const qqs_font_t *font = &F_M;
    bool enabled = true;
  };
  void button(int16_t x, int16_t y, int16_t w, int16_t h, const Btn &b, screenFunc_t cb);

  // Common chrome
  void top_bar();                                        // status text + Wi-Fi IP, y 0..21
  void header(const char *title, screenFunc_t back);     // back button (none if back is null) + title, y 0..31
  // Centred question box over the current screen (clears the touch controls first: modal)
  void ask_box(const char *title, const char *l1, const char *l2, const Btn &no, screenFunc_t no_cb, const Btn &yes, screenFunc_t yes_cb);
  constexpr int16_t TOP_H = 22, HEADER_H = 32;

  // Number formatting helpers (static buffers, use immediately)
  const char *fmt_int(int32_t v, const char *suffix = nullptr);
  const char *fmt_signed2(float v);                      // "+0.06"
  const char *fmt_hm(uint32_t seconds);                  // "1:23" (h:mm) or "12m"

} // namespace qqs
