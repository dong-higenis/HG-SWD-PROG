#ifndef PROG_INSPECT_H
#define PROG_INSPECT_H

#include <stdbool.h>
#include <stdint.h>

/* Read-only, bounded file access; no target or filesystem dependency. */
typedef struct
{
  void    *ctx;
  uint32_t size;
  bool (*read)(void *ctx, uint32_t offset, void *data, uint32_t size);
} prog_file_t;

typedef struct
{
  char     name[48];
  char     algo[96];
  char     image[48];
  uint32_t address;
  uint32_t crc32;
  uint32_t loader_crc32;
} prog_manifest_t;

typedef struct
{
  uint32_t flash_base;
  uint32_t flash_size;
  uint32_t page_hint;
  uint32_t sectors;
  uint32_t sector_size;
  uint32_t code_start;
  uint32_t code_end;
  uint32_t init;
  uint32_t write;
  uint32_t erase;
  uint32_t mass_erase;
  uint32_t code_offset;
  uint32_t code_filesz;
} prog_loader_t;

typedef struct
{
  uint32_t sp;
  uint32_t reset;
  char     version[33];
  char     name[33];
  bool     has_version;
} prog_image_t;

const char *progParseManifest(char *text, prog_manifest_t *out);
const char *progInspectLoader(const prog_file_t *file, prog_loader_t *out);
const char *progInspectImage(const prog_file_t *file, uint32_t address, prog_image_t *out);
bool        progFileCrc32(const prog_file_t *file, uint32_t *crc);

#endif
