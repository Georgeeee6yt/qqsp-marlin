/**
 * QQS-Pro v3 UI — drawing helpers.
 */
#include "../../../inc/MarlinConfigPre.h"

#if ENABLED(QQS_UI)

#include "qqs_draw.h"
#include "../tft.h"
#include "../touch.h"
#include "../../marlinui.h"

#ifdef MKS_WIFI
  extern uint8_t mks_wifi_ip[4];
  extern uint8_t mks_wifi_net_state;
#endif

// ---------------------------------------------------------------------------
// Font primitives (also used by canvas.cpp)
// ---------------------------------------------------------------------------

const qqs_glyph_t *qqs_find_glyph(const qqs_font_t *font, uint16_t cp) {
  int16_t lo = 0, hi = int16_t(font->count) - 1;
  while (lo <= hi) {
    const int16_t mid = (lo + hi) >> 1;
    const uint16_t c = font->glyphs[mid].cp;
    if (c == cp) return &font->glyphs[mid];
    if (c < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

uint16_t qqs_utf8_next(const uint8_t *&s) {
  uint8_t c = *s;
  if (!c) return 0;
  s++;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) {
    if ((*s & 0xC0) != 0x80) return '?';
    return uint16_t(((c & 0x1F) << 6) | (*s++ & 0x3F));
  }
  if ((c & 0xF0) == 0xE0) {
    if ((s[0] & 0xC0) != 0x80 || (s[1] & 0xC0) != 0x80) return '?';
    const uint16_t cp = uint16_t(((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F));
    s += 2;
    return cp;
  }
  // 4-byte sequences (outside the BMP) and stray bytes: skip continuation bytes
  while ((*s & 0xC0) == 0x80) s++;
  return '?';
}

uint16_t qqs_text_width(const qqs_font_t *font, const char *str) {
  const uint8_t *s = (const uint8_t *)str;
  uint16_t w = 0;
  for (uint16_t cp; (cp = qqs_utf8_next(s));) {
    const qqs_glyph_t *g = qqs_find_glyph(font, cp);
    if (!g) g = qqs_find_glyph(font, '?');
    if (g) w += g->adv;
  }
  return w;
}

namespace qqs {

  void canvas(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t bg) {
    tft.canvas(x, y, w, h);
    tft.set_background(bg);
  }

  void text(int16_t x, int16_t y, const qqs_font_t &f, uint16_t color, uint16_t bg, const char *s) {
    if (s && *s) tft.queue.add_qtext(x, y, &f, color, bg, s);
  }

  void text_c(int16_t x, int16_t y, int16_t w, const qqs_font_t &f, uint16_t color, uint16_t bg, const char *s) {
    text(x + (w - int16_t(width(f, s))) / 2, y, f, color, bg, s);
  }

  void text_r(int16_t xr, int16_t y, const qqs_font_t &f, uint16_t color, uint16_t bg, const char *s) {
    text(xr - int16_t(width(f, s)), y, f, color, bg, s);
  }

  void bitmap(int16_t x, int16_t y, const qqs_bitmap_t &b, uint16_t color, uint16_t bg) {
    tft.queue.add_qbitmap(x, y, &b, color, bg);
  }

  void frame(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) { tft.add_rectangle(x, y, w, h, color); }
  void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)  { tft.add_bar(x, y, w, h, color); }

  void button(int16_t x, int16_t y, int16_t w, int16_t h, const Btn &b, screenFunc_t cb) {
    const uint16_t fg = b.enabled ? b.fg : C_DIM,
                   border = b.enabled ? b.border : C_TILE;
    canvas(x, y, w, h, b.bg);
    frame(0, 0, w, h, border);

    const int16_t lh = b.label ? b.font->height : 0,
                  sh = b.sub ? F_S.height : 0,
                  ih = b.icon ? b.icon->h + 3 : 0,
                  total = ih + lh + sh;
    int16_t cy = (h - total) / 2;
    if (b.icon) { bitmap((w - b.icon->w) / 2, cy, *b.icon, fg, b.bg); cy += ih; }
    if (b.label) { text_c(0, cy, w, *b.font, fg, b.bg, b.label); cy += lh; }
    if (b.sub) text_c(0, cy, w, F_S, b.enabled ? C_MUTED : C_DIM, b.bg, b.sub);

    if (b.enabled && cb) touch.add_control(BUTTON, x, y, w, h, (intptr_t)cb);
  }

  void top_bar() {
    canvas(0, 0, TFT_WIDTH, TOP_H, C_BAR);

    // Right side: Wi-Fi state / IP
    char ip[20];
    const qqs_bitmap_t *icon = &qqs_i18_wifi_off;
    uint16_t icolor = C_DIM;
    const char *label = "Wi-Fi 未連線";
    #ifdef MKS_WIFI
      if (mks_wifi_net_state == 1) {
        sprintf_P(ip, PSTR("%u.%u.%u.%u"), mks_wifi_ip[0], mks_wifi_ip[1], mks_wifi_ip[2], mks_wifi_ip[3]);
        label = ip; icon = &qqs_i18_wifi; icolor = C_OK;
      }
      else if (mks_wifi_net_state == 2) label = "Wi-Fi 異常";
    #endif
    const int16_t lw = width(F_S, label), rx = TFT_WIDTH - 6 - lw;
    const int16_t ix = rx - icon->w - 4;

    // Left side: status message, clipped before the IP block
    // (a long message is simply cut off where the IP block starts)
    const char *msg = ui.status_message;
    if (msg && *msg) text(6, 3, F_S, C_TEXT, C_BAR, msg);
    fill(ix - 8, 0, TFT_WIDTH - (ix - 8), TOP_H, C_BAR);

    bitmap(ix, (TOP_H - icon->h) / 2, *icon, icolor, C_BAR);
    text(rx, 3, F_S, C_MUTED, C_BAR, label);
  }

  void header(const char *title, screenFunc_t back) {
    canvas(0, 0, TFT_WIDTH, HEADER_H, C_BAR);
    bitmap(6, (HEADER_H - qqs_i18_chevron_left.h) / 2, qqs_i18_chevron_left, C_TEXT, C_BAR);
    text(26, (HEADER_H - F_M.height) / 2, F_M, C_TEXT, C_BAR, "返回");
    text_c(0, (HEADER_H - F_M.height) / 2, TFT_WIDTH, F_M, C_TEXT, C_BAR, title);
    fill(0, HEADER_H - 1, TFT_WIDTH, 1, C_BORDER);
    if (back) touch.add_control(BUTTON, 0, 0, 84, HEADER_H + 6, (intptr_t)back);
  }

  const char *fmt_int(int32_t v, const char *suffix) {
    static char buf[16];
    if (suffix) sprintf_P(buf, PSTR("%ld%s"), long(v), suffix);
    else        sprintf_P(buf, PSTR("%ld"), long(v));
    return buf;
  }

  const char *fmt_signed2(float v) {
    static char buf[12];
    int32_t h = lroundf(v * 100.0f);
    const char sign = h < 0 ? '-' : '+';
    if (h < 0) h = -h;
    sprintf_P(buf, PSTR("%c%ld.%02ld"), sign, long(h / 100), long(h % 100));
    return buf;
  }

  const char *fmt_hm(uint32_t s) {
    static char buf[12];
    const uint32_t m = (s + 30) / 60;
    sprintf_P(buf, PSTR("%lu:%02lu"), (unsigned long)(m / 60), (unsigned long)(m % 60));
    return buf;
  }

} // namespace qqs

#endif // QQS_UI
