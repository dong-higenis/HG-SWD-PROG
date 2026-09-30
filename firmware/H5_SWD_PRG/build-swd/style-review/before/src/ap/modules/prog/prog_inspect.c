#include "prog_inspect.h"
#include <stddef.h>
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static bool range(uint32_t off, uint32_t n, uint32_t limit) { return off <= limit && n <= limit - off; }
static bool readAt(const prog_file_t *f, uint32_t off, void *p, uint32_t n) {
  return range(off, n, f->size) && f->read(f->ctx, off, p, n);
}
static char *trim(char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r') p++;
  size_t n = strlen(p);
  while (n && (p[n-1] == ' ' || p[n-1] == '\t' || p[n-1] == '\r')) p[--n] = 0;
  return p;
}
static bool copy(char *out, size_t size, const char *in) {
  size_t n = strlen(in);
  if (!n || n >= size) return false;
  memcpy(out, in, n + 1);
  return true;
}
static bool hex32(const char *p, uint32_t *out) {
  if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) return false;
  p += 2;
  uint32_t v = 0;
  unsigned n = 0;
  for (; *p; p++, n++) {
    unsigned d;
    if (*p >= '0' && *p <= '9') d = (unsigned)(*p - '0');
    else if (*p >= 'a' && *p <= 'f') d = (unsigned)(*p - 'a') + 10;
    else if (*p >= 'A' && *p <= 'F') d = (unsigned)(*p - 'A') + 10;
    else return false;
    if (n == 8) return false;
    v = (v << 4) | d;
  }
  *out = v;
  return n != 0;
}
const char *progParseManifest(char *text, prog_manifest_t *out) {
  memset(out, 0, sizeof(*out));
  unsigned seen = 0;
  for (char *line = text; line && *line;) {
    char *next = strchr(line, '\n');
    if (next) *next++ = 0;
    char *key = trim(line);
    line = next;
    if (!*key || *key == '#') continue;
    char *v = strchr(key, '=');
    if (!v) return "fw.txt: expected key=value";
    *v++ = 0;
    key = trim(key); v = trim(v);
    unsigned bit;
    if (!strcmp(key, "name")) {
      bit = 1; if (!copy(out->name, sizeof(out->name), v)) return "fw.txt: invalid name";
      for (const char *p = v; *p; p++)
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126) return "fw.txt: name must be printable ASCII";
    } else if (!strcmp(key, "device")) {
      bit = 2; if (strcmp(v, "STM32H72x/STM32H73x")) return "fw.txt: wrong device";
    } else if (!strcmp(key, "algo")) {
      bit = 4;
      if (strcmp(v, "/prog/loaders/st/0x483.stldr")) return "fw.txt: unsupported loader path";
      if (!copy(out->algo, sizeof(out->algo), v)) return "fw.txt: invalid algo";
    } else if (!strcmp(key, "image")) {
      bit = 8;
      char *addr = strchr(v, '@');
      if (!addr) return "fw.txt: image needs @ address";
      *addr++ = 0; v = trim(v);
      if (!copy(out->image, sizeof(out->image), v) || strstr(v, "..")) return "fw.txt: invalid image name";
      for (const char *p = v; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return "fw.txt: image must be a basename";
      if (!hex32(trim(addr), &out->address)) return "fw.txt: invalid image address";
    } else if (!strcmp(key, "crc32")) {
      bit = 16; if (!hex32(v, &out->crc32)) return "fw.txt: invalid crc32";
    } else if (!strcmp(key, "loader_crc32")) {
      bit = 32; if (!hex32(v, &out->loader_crc32)) return "fw.txt: invalid loader_crc32";
    } else return "fw.txt: unknown key";
    if (seen & bit) return "fw.txt: duplicate key";
    seen |= bit;
  }
  return seen == 63 ? NULL : "fw.txt: missing field";
}

typedef struct { uint32_t off, addr, filesz, memsz, flags; } segment_t;

const char *progInspectLoader(const prog_file_t *f, prog_loader_t *out) {
  uint8_t h[52], b[200];
  segment_t seg[16];
  unsigned ns = 0;
  memset(out, 0, sizeof(*out));
  if (!readAt(f, 0, h, sizeof(h))) return "loader: truncated ELF header";
  if (memcmp(h, "\177ELF\1\1\1", 7) || u16(h+16) != 2 || u16(h+18) != 40 || u32(h+20) != 1 ||
      u16(h+40) != 52 || u16(h+42) != 32 || u16(h+46) != 40) return "loader: expected ELF32 LE ARM executable";
  uint32_t ph = u32(h+28), sh = u32(h+32);
  unsigned np = u16(h+44), nh = u16(h+48);
  if (!np || np > 16 || !nh || nh > 256 || !range(ph, np*32, f->size) || !range(sh, nh*40, f->size))
    return "loader: invalid ELF tables";
  unsigned code_count = 0;
  for (unsigned i = 0; i < np; i++) {
    if (!readAt(f, ph+i*32, b, 32)) return "loader: program header read failed";
    if (u32(b) != 1) continue;
    segment_t s = {u32(b+4), u32(b+8), u32(b+16), u32(b+20), u32(b+24)};
    if (!s.memsz || s.filesz > s.memsz || !range(s.off, s.filesz, f->size) ||
        s.addr != u32(b+12) || s.memsz > UINT32_MAX - s.addr) return "loader: invalid load segment";
    for (unsigned j = 0; j < ns; j++)
      if (s.addr < seg[j].addr + seg[j].memsz && seg[j].addr < s.addr + s.memsz) return "loader: overlapping segments";
    if (s.addr == 0 && s.flags == 4 && s.filesz == 200 && s.memsz == 200) {
      /* StorageInfo is metadata at address zero, never executable target RAM. */
    } else if (s.addr >= 0x24000000 && s.addr < 0x24020000 && s.memsz <= 0x24020000-s.addr && (s.flags & 1)) {
      if (++code_count != 1) return "loader: only one AXI code segment supported";
      out->code_start = s.addr; out->code_end = s.addr + s.memsz;
      out->code_offset = s.off; out->code_filesz = s.filesz;
    } else return "loader: unsupported segment layout";
    seg[ns++] = s;
  }
  if (code_count != 1) return "loader: missing AXI code";
  const char *names[] = {"StorageInfo", "Init", "Write", "SectorErase", "MassErase"};
  uint32_t values[5] = {0};
  unsigned found = 0;
  for (unsigned i = 0; i < nh; i++) {
    if (!readAt(f, sh+i*40, b, 40)) return "loader: section read failed";
    if (u32(b+4) != 2) continue;
    uint32_t off = u32(b+16), size = u32(b+20), link = u32(b+24);
    if (u32(b+36) != 16 || size % 16 || size/16 > 4096 || !range(off, size, f->size) || link >= nh)
      return "loader: invalid symbol table";
    if (!readAt(f, sh+link*40, b, 40) || u32(b+4) != 3) return "loader: invalid string table";
    uint32_t str = u32(b+16), strsz = u32(b+20);
    if (!range(str, strsz, f->size)) return "loader: string table out of file";
    for (uint32_t j = 0; j < size; j += 16) {
      uint8_t sym[16];
      if (!readAt(f, off+j, sym, 16)) return "loader: symbol read failed";
      if ((sym[12] >> 4) != 1 || u16(sym+14) == 0) continue;
      uint32_t ni = u32(sym);
      if (ni >= strsz) return "loader: invalid symbol name";
      for (unsigned k = 0; k < 5; k++) {
        uint32_t n = (uint32_t)strlen(names[k]) + 1;
        if (n > strsz-ni) continue;
        if (!readAt(f, str+ni, b, n)) return "loader: name read failed";
        if (memcmp(b, names[k], n)) continue;
        if (u16(sym+14) >= nh) return "loader: invalid required symbol section";
        if (found & (1U << k)) return "loader: duplicate required symbol";
        if ((sym[12] & 15) != (k ? 2 : 1)) return "loader: wrong symbol type";
        uint32_t value = u32(sym+4), symsize = u32(sym+8);
        bool mapped = false;
        for (unsigned z = 0; z < ns; z++) {
          uint32_t addr = k ? value & ~1U : value;
          if (addr >= seg[z].addr && range(addr-seg[z].addr, k ? (symsize ? symsize : 2) : 200, seg[z].filesz) &&
              (k ? ((value & 1) && (seg[z].flags & 1)) : (seg[z].addr == 0 && symsize == 200))) mapped = true;
        }
        if (!mapped) return "loader: required symbol outside expected segment";
        found |= 1U << k; values[k] = value;
      }
    }
  }
  if ((found & 15) != 15) return "loader: missing StorageInfo/Init/Write/SectorErase";
  bool info = false;
  for (unsigned i = 0; i < ns; i++) {
    if (seg[i].addr == 0 && values[0] == 0) {
      if (!readAt(f, seg[i].off, b, 200)) return "loader: StorageInfo read failed";
      info = true; break;
    }
  }
  if (!info || memcmp(b, "0x483\0", 6) || u16(b+100) != 1 || u32(b+104) != 0x08000000 ||
      u32(b+108) != 0x100000 || u32(b+112) != 4096 || b[116] != 0xFF ||
      u32(b+120) != 8 || u32(b+124) != 0x20000) return "loader: not supported H723 1MiB geometry";
  for (unsigned i = 128; i < 200; i++) if (b[i]) return "loader: unexpected sector table";
  out->flash_base = u32(b+104); out->flash_size = u32(b+108); out->page_hint = u32(b+112);
  out->sectors = 8; out->sector_size = 0x20000;
  out->init = values[1]; out->write = values[2]; out->erase = values[3]; out->mass_erase = values[4];
  return NULL;
}

const char *progInspectImage(const prog_file_t *f, uint32_t address, prog_image_t *out) {
  uint8_t b[72];
  memset(out, 0, sizeof(*out));
  /* This initial workflow accepts complete H723 images, starting at the vector table. */
  if (address != 0x08000000 || f->size < 8 || f->size > 0x100000) return "image: expected complete BIN at 0x08000000, <=1MiB";
  if (!readAt(f, 0, b, 8)) return "image: vector read failed";
  out->sp = u32(b); out->reset = u32(b+4);
  uint32_t sp = out->sp, pc = out->reset & ~1U;
  bool ram = (sp > 0x20000000 && sp <= 0x20020000) || (sp > 0x24000000 && sp <= 0x24050000) ||
             (sp > 0x30000000 && sp <= 0x30008000) || (sp > 0x38000000 && sp <= 0x38004000);
  if (!ram || (sp & 7)) return "image: invalid H723 initial stack";
  if (!(out->reset & 1) || pc < address || pc >= address+f->size) return "image: reset vector outside BIN or not Thumb";
  if (f->size >= 0x400 + sizeof(b)) {
    if (!readAt(f, 0x400, b, sizeof(b))) return "image: version read failed";
    if (u32(b) == 0x56455220) {
      if (u32(b+68) != address) return "image: embedded address disagrees with fw.txt";
      memcpy(out->version, b+4, 32); memcpy(out->name, b+36, 32); out->has_version = true;
    }
  }
  return NULL;
}

bool progFileCrc32(const prog_file_t *f, uint32_t *crc) {
  uint8_t buf[512];
  uint32_t v = UINT32_MAX;
  for (uint32_t off = 0; off < f->size;) {
    uint32_t n = f->size-off;
    if (n > sizeof(buf)) n = sizeof(buf);
    if (!readAt(f, off, buf, n)) return false;
    for (uint32_t i = 0; i < n; i++) {
      v ^= buf[i];
      for (unsigned j = 0; j < 8; j++) v = (v >> 1) ^ (0xEDB88320U & (0U - (v & 1)));
    }
    off += n;
  }
  *crc = ~v;
  return true;
}
