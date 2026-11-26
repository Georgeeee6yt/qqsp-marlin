/**
 * QQS-Pro: what the Wi-Fi module's web page asks the board (module firmware QQS1.2+).
 *
 * These come in as G-code lines from the module and are answered right in the module's input parser
 * (qqs_wifi_direct), not through the command queue, so the page gets an answer even while a command runs:
 *   M2012          status ("QQS s=…", "QQSN <name>"), see qqs::wifi_status()
 *   M2013 H<hex>   list a folder (hex of its 8.3 path, "" = root):
 *                    "QQSF <d|f> <size> <hex 8.3 name> <name in UTF-8>" per entry, in directory order,
 *                    then "QQSE <count> <code>"  0 ok, 1 cut at LIST_MAX, 3 no such folder, 4 no card, 5 bad argument
 *                  The lines go out a few at a time from idle() (the link is 115200 baud: ~10 KB/s).
 *   M2014 H<hex>   start printing a file (hex of its 8.3 path from the root):
 *                    "QQSP <code>"  (only *.G* files; others = 3)  0 started, 1 a job is open, 2 printer busy (moving / not on the home or
 *                    file screens), 3 no such file, 4 no card, 5 bad argument
 *
 * While an SD print waits for its temperatures (M109 / M190) Marlin runs only idle(), so nothing reads the
 * module: qqs_wifi_task() reads it then, answers the module's status queries directly and queues the rest.
 */

#include "../../inc/MarlinConfig.h"

#if ALL(MKS_WIFI, QQS_UI)

#include "../../MarlinCore.h"
#include "../../gcode/queue.h"
#include "../../sd/cardreader.h"
#include "../planner.h"
#include "mks_wifi.h"
#include "mks_wifi_gcodes.h"
#include "qqs_espflash.h"
#include "qqs_wifi_web.h"

namespace qqs { bool wifi_can_start(); void wifi_started(); }

bool qqs_wifi_idle_ctx = false;

static void out(const char *s) { mks_wifi_out_add((uint8_t *)s, strlen(s)); }

// "H2F41" -> "/A". Empty argument -> "". False if not hex or too long.
static bool unhex(const char *a, char *o, const size_t cap) {
  while (*a == ' ') a++;
  if (*a == 'H') a++;
  size_t n = 0;
  auto nib = [](const char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  while (a[0] && a[0] != ' ' && a[0] != '\r') {
    const int h = nib(a[0]), l = a[1] ? nib(a[1]) : -1;
    if (h < 0 || l < 0 || n + 1 >= cap) return false;
    const char c = char((h << 4) | l);
    if (!c) return false;
    o[n++] = c;
    a += 2;
  }
  o[n] = '\0';
  return true;
}

// ---- M2013: folder list ----

#define LIST_MAX 150
#define NAME_MAX 300

static struct { bool on; uint16_t n; MediaFile dir; } L;

static void list_start(const char *arg) {
  char path[80];
  L.on = false;
  if (!unhex(arg, path, sizeof(path))) { out("QQSE 0 5\n"); return; }
  if (!card.isMounted() && !printingIsActive() && !printingIsPaused()) card.mount();   // no card-detect pin
  if (!card.isMounted()) { out("QQSE 0 4\n"); return; }
  if (!CardReader::qqs_open_dir(L.dir, path)) { out("QQSE 0 3\n"); return; }
  L.n = 0;
  L.on = true;
}

static void list_pump() {
  for (uint8_t k = 0; L.on && k < 4; k++) {
    if (MYSERIAL2.availableForWrite() < 160) return;   // let the UART drain; never block the main loop
    if (!card.isMounted()) { L.on = false; out("QQSE 0 4\n"); return; }
    dir_t p;
    char name[NAME_MAX];
    const bool more = L.n < LIST_MAX && CardReader::qqs_next(L.dir, p, name, sizeof(name));
    if (!more) {
      char b[24];
      sprintf_P(b, PSTR("QQSE %u %u\n"), L.n, L.n >= LIST_MAX ? 1 : 0);
      out(b);
      L.on = false;
      return;
    }
    char dos[13];
    CardReader::qqs_83(dos, p);
    char b[40 + NAME_MAX];
    char *w = b + sprintf_P(b, PSTR("QQSF %c %lu "), DIR_IS_SUBDIR(&p) ? 'd' : 'f', (unsigned long)p.fileSize);
    for (const char *c = dos; *c; c++) w += sprintf_P(w, PSTR("%02X"), uint8_t(*c));
    *w++ = ' ';
    strcpy(w, name);
    strcat(w, "\n");
    out(b);
    L.n++;
  }
}

// ---- M2014: start a print ----

static bool file_exists(const char *path) {
  const char * const slash = strrchr(path, '/');
  char parent[64];
  const char *fname = path;
  parent[0] = '\0';
  if (slash) {
    const size_t n = slash - path;
    if (n >= sizeof(parent)) return false;
    memcpy(parent, path, n); parent[n] = '\0';
    fname = slash + 1;
  }
  MediaFile dir, f;
  if (!*fname || !CardReader::qqs_open_dir(dir, parent)) return false;
  if (!f.open(&dir, fname, O_READ)) return false;
  const bool ok = !f.isDir();
  f.close();
  return ok;
}

static void print_start(const char *arg) {
  char path[64];
  uint8_t r;
  const char *dot = nullptr;
  const bool ok = unhex(arg, path, sizeof(path)) && path[0] == '/' && strlen(path) <= 50;   // M23 injection limit
  if (ok) { dot = strrchr(path, '.'); if (dot && strchr(dot, '/')) dot = nullptr; }
  if (!ok) r = 5;
  else if (!dot || (dot[1] | 0x20) != 'g') r = 3;   // only G-code (*.G*), like the lists: "not found"
  else if (printingIsActive() || printingIsPaused() || card.isFileOpen()) r = 1;
  else if (qqs_wifi_idle_ctx || planner.has_blocks_queued() || !qqs::wifi_can_start()) r = 2;
  else {
    if (!card.isMounted()) card.mount();
    if (!card.isMounted()) r = 4;
    else if (!file_exists(path)) r = 3;
    else { card.openAndPrintFile(path); qqs::wifi_started(); r = 0; }
  }
  char b[12];
  sprintf_P(b, PSTR("QQSP %u\n"), r);
  out(b);
}

// ---- dispatch ----

static bool is(const char *line, PGM_P cmd) {
  const size_t n = strlen_P(cmd);
  return !strncmp_P(line, cmd, n) && (line[n] == '\0' || line[n] == ' ' || line[n] == '\r');
}

bool qqs_wifi_direct(const char *line) {
  if (is(line, PSTR("M2012"))) { mks_m2012(); return true; }
  if (is(line, PSTR("M2013"))) { list_start(line + 5); return true; }
  if (is(line, PSTR("M2014"))) { print_start(line + 5); return true; }
  if (qqs_wifi_idle_ctx) {   // the module's own status queries, while the queue is stuck in a heat wait
    if (is(line, PSTR("M997"))) { mks_m997(); return true; }
    if (is(line, PSTR("M991"))) { mks_m991(); return true; }
    if (is(line, PSTR("M992"))) { mks_m992(); return true; }
    if (is(line, PSTR("M994"))) { mks_m994(); return true; }
    if (is(line, PSTR("M115"))) { mks_m115(); return true; }
    if (is(line, PSTR("M27")))  { mks_m27();  return true; }
  }
  return false;
}

void qqs_wifi_task() {
  static bool busy = false;
  if (busy || espf_busy()) return;
  busy = true;
  // Reading the module from idle() only in an SD print's heat wait: the module does not start uploads while
  // the board says it is printing (and mks_wifi_input refuses them then), and nothing else is reading it.
  if (wait_for_heatup && CardReader::isPrinting() && !mks_wifi_in_input()) {
    qqs_wifi_idle_ctx = true;
    for (uint16_t n = 0; n < 256 && MYSERIAL2.available() > 0; n++) mks_wifi_input(MYSERIAL2.read());
    qqs_wifi_idle_ctx = false;
  }
  list_pump();
  busy = false;
}

#endif // MKS_WIFI && QQS_UI
