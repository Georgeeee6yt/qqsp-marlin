#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <strings.h>
#include <vector>
#define PACKED __attribute__((packed))
struct dir_t { uint8_t name[11]; uint8_t attributes; uint8_t reservedNT; uint8_t ct10; uint16_t ct, cd, ad, fch, wt, wd, fcl; uint32_t fileSize; } PACKED;
struct vfat_t { uint8_t sequenceNumber; uint16_t name1[5]; uint8_t attributes, reservedNT, checksum; uint16_t name2[6]; uint16_t firstClusterLow; uint16_t name3[2]; } PACKED;
static_assert(sizeof(dir_t) == 32 && sizeof(vfat_t) == 32, "size");
const uint8_t DIR_NAME_FREE = 0, DIR_NAME_DELETED = 0xE5, DIR_ATT_HIDDEN = 2, DIR_ATT_VOLUME_ID = 8;
static inline uint8_t DIR_IS_LONG_NAME(const dir_t *d) { return (d->attributes & 0x3F) == 0x0F; }
static inline uint8_t DIR_IS_SUBDIR(const dir_t *d) { return (d->attributes & 0x18) == 0x10; }
#define WITHIN(v,a,b) ((v)>=(a)&&(v)<=(b))
#define _MIN(a,b) ((a)<(b)?(a):(b))
#define QQS_LFN_SEQ 8
#define QQS_JOB_LFN 160
struct MediaFile { std::vector<uint8_t> b; size_t pos = 0;
  int16_t read(void *o, uint16_t n) { if (pos + n > b.size()) return 0; memcpy(o, &b[pos], n); pos += n; return n; }
  void rewind() { pos = 0; } };
char *createFilename(char * const buffer, const dir_t &p) { char *pos = buffer; for (int i = 0; i < 11; i++) { if (p.name[i] == ' ') continue; if (i == 8) *pos++ = '.'; *pos++ = p.name[i]; } *pos++ = 0; return buffer; }
struct CardReader { static bool qqs_next(MediaFile &d, dir_t &p, char *name, const uint16_t cap); static void qqs_83(char *b, const dir_t &p) { createFilename(b, p); } };
#include "extract.inc"   // run.sh cuts qqs_next() etc. out of Marlin/src/sd/cardreader.cpp
int main(int argc, char **argv) {
  MediaFile d; FILE *f = fopen(argv[1], "rb"); int c; while ((c = fgetc(f)) != EOF) d.b.push_back(c); fclose(f);
  const uint16_t cap = argc > 2 ? atoi(argv[2]) : 300;
  dir_t p; char n[400], dos[13];
  while (CardReader::qqs_next(d, p, n, cap)) { CardReader::qqs_83(dos, p); printf("%c %u %-12s [%s] %zu\n", DIR_IS_SUBDIR(&p) ? 'd' : 'f', p.fileSize, dos, n, strlen(n)); }
  if (argc > 3) { char out[QQS_JOB_LFN]; qqs_find_lfn(d, argv[3], out, sizeof(out)); printf("find %s -> [%s]\n", argv[3], out); }
}
