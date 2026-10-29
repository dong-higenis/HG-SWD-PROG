#include "prog_catalog.h"
#include <stdio.h>
#include <string.h>

static prog_package_t scratch; /* Scan buffer; the caller owns the SD, so one scan runs at a time. */

bool progCatalogIsValidId(const char *id)
{
  size_t n = strlen(id);
  if (!n || n >= PROG_ID_SIZE)
  {
    return false;
  }
  for (; *id; id++)
  {
    if (!((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') || (*id >= '0' && *id <= '9') || *id == '_' ||
          *id == '-'))
    {
      return false;
    }
  }
  return true;
}

static char *progCatalogTrim(char *s)
{
  s        += strspn(s, " \t\r");
  size_t n  = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
  {
    s[--n] = 0;
  }
  return s;
}

/* Value of the first "device=" line, trimmed; the parsers below reject duplicates. */
static void progCatalogDevice(const char *text, char *out, size_t size)
{
  out[0] = 0;
  for (const char *line = text; line;)
  {
    const char *v = line + strspn(line, " \t");
    line          = strchr(line, '\n');
    line          = line ? line + 1 : NULL;
    if (strncmp(v, "device", 6))
    {
      continue;
    }
    v += 6;
    v += strspn(v, " \t");
    if (*v++ != '=')
    {
      continue;
    }
    v        += strspn(v, " \t");
    size_t n  = strcspn(v, "\r\n");
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
    {
      n--;
    }
    if (n < size)
    {
      memcpy(out, v, n);
      out[n] = 0;
    }
    return;
  }
}

/* Whole small text file, NUL-terminated; text holds 1025 bytes. Returns NULL or a short reason. */
static const char *progCatalogReadText(const char *path, char *text, FRESULT *io)
{
  FIL file;
  if ((*io = f_open(&file, path, FA_READ)) != FR_OK)
  {
    return "open";
  }
  const char *error = NULL;
  UINT        got   = 0;
  uint32_t    size  = (uint32_t)f_size(&file);
  if (!size || size > 1024)
  {
    error = "size";
  }
  else if ((*io = f_read(&file, text, size, &got)) != FR_OK || got != size)
  {
    error = "read";
  }
  else if (memchr(text, 0, size))
  {
    error = "NUL";
  }
  FRESULT closed = f_close(&file);
  if (!error && closed != FR_OK)
  {
    *io   = closed;
    error = "close";
  }
  text[error ? 0 : size] = 0;
  return error;
}

/* x.y.z or x.y.z-tag, as the Hub names version folders. */
static bool progCatalogIsVersion(const char *v)
{
  if (strlen(v) >= PROG_VERSION_SIZE)
  {
    return false;
  }
  for (unsigned part = 0; part < 3; part++)
  {
    size_t digits = strspn(v, "0123456789");
    if (!digits)
    {
      return false;
    }
    v += digits;
    if (part < 2 && *v++ != '.')
    {
      return false;
    }
  }
  if (*v == '-')
  {
    v++;
    size_t tag = strspn(v, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.-");
    return tag && !v[tag];
  }
  return !*v;
}

/* Hub release.txt (key=value, schema=1). Informational: on any doubt the whole file is ignored and device= decides.
 * Unknown keys, blank lines and # comments are skipped; CRLF is accepted. */
static void progCatalogRelease(char *text, prog_package_t *out)
{
  static const char *keys[] = {"schema", "id", "chip", "version", "date"};
  char              *values[5] = {0};
  for (char *line = text; line && *line;)
  {
    char *next = strchr(line, '\n');
    if (next)
    {
      *next++ = 0;
    }
    char *key = progCatalogTrim(line);
    line      = next;
    char *v   = strchr(key, '=');
    if (!*key || *key == '#' || !v)
    {
      continue;
    }
    *v++ = 0;
    key  = progCatalogTrim(key);
    for (unsigned k = 0; k < 5; k++)
    {
      if (!strcmp(key, keys[k]))
      {
        if (values[k])
        {
          return; /* Duplicate key. */
        }
        values[k] = progCatalogTrim(v);
      }
    }
  }
  if (!values[0] || strcmp(values[0], "1") || (values[2] && strlen(values[2]) >= sizeof(out->chip)))
  {
    return;
  }
  out->release = true;
  if (values[2])
  {
    strcpy(out->chip, values[2]);
  }
  if (values[3] && progCatalogIsVersion(values[3]))
  {
    strcpy(out->version, values[3]);
  }
}

const char *progPackageLoad(const char *id, prog_package_t *out, FRESULT *io)
{
  memset(out, 0, sizeof(*out));
  out->target = PROG_TARGET_NONE;
  *io         = FR_OK;
  if (!progCatalogIsValidId(id))
  {
    return "Invalid project ID";
  }
  char    path[64];
  char    text[1025];
  FRESULT ignored;
  snprintf(path, sizeof(path), "/prog/fw/%s/release.txt", id);
  if (!progCatalogReadText(path, text, &ignored))
  {
    progCatalogRelease(text, out);
  }
  snprintf(path, sizeof(path), "/prog/fw/%s/fw.txt", id);
  const char *error = progCatalogReadText(path, text, io);
  if (error)
  {
    static const struct
    {
      const char *reason, *message;
    } messages[] = {{"open", "fw.txt open"}, {"size", "fw.txt size"}, {"read", "fw.txt read"},
                    {"NUL", "fw.txt NUL"},   {"close", "fw.txt close"}};
    for (unsigned i = 0; i < sizeof(messages) / sizeof(messages[0]); i++)
    {
      if (!strcmp(error, messages[i].reason))
      {
        return messages[i].message;
      }
    }
    return error;
  }
  /* release.txt chip= is trusted only when that chip uses this device=; without chip=, device= decides. */
  char device[32];
  progCatalogDevice(text, device, sizeof(device));
  const char *unknown = "Unknown device";
  if (out->chip[0])
  {
    out->target                    = progTargetFromChip(out->chip);
    const prog_target_info_t *info = progTargetInfo(out->target);
    if (!info)
    {
      unknown = "Unknown chip";
    }
    else if (strcmp(info->device, device))
    {
      out->target = PROG_TARGET_NONE;
      unknown     = "Chip mismatch";
    }
  }
  else
  {
    out->target = progTargetFromDevice(device);
  }
  out->esp_project = espProjectIsManifest(text);
  if (!out->esp_project)
  {
    error = progParseManifest(text, &out->manifest);
  }
  else if (!(error = espProjectParse(text, &out->esp)))
  {
    strcpy(out->manifest.name, out->esp.name);
  }
  /* A package no chip owns cannot be written; say why rather than the parser's first complaint. */
  return out->target == PROG_TARGET_NONE ? unknown : error;
}

/* The BIN files fw.txt names must exist with a plausible size before the package is offered. */
static const char *progCatalogImages(const char *id, const prog_package_t *package, prog_catalog_entry_t *entry)
{
  char    path[128];
  FILINFO info;
  if (!package->esp_project)
  {
    snprintf(path, sizeof(path), "/prog/fw/%s/%s", id, package->manifest.image);
    if (f_stat(path, &info) != FR_OK || (info.fattrib & AM_DIR))
    {
      return "Image missing";
    }
    if (info.fsize < 8 || info.fsize > 0x100000)
    {
      return "Invalid image size";
    }
    entry->images = 1;
    entry->size   = (uint32_t)info.fsize;
    return NULL;
  }
  for (unsigned i = 0; i < package->esp.count; i++)
  {
    const esp_project_image_t *image = &package->esp.images[i];
    snprintf(path, sizeof(path), "/prog/fw/%s/%s", id, image->file);
    if (f_stat(path, &info) != FR_OK || (info.fattrib & AM_DIR))
    {
      return "ESP image missing";
    }
    if (info.fsize != image->size)
    {
      return "ESP image size";
    }
  }
  entry->images = (uint8_t)package->esp.count;
  entry->size   = package->esp.total;
  return NULL;
}

static int progCatalogCompare(const prog_catalog_entry_t *a, const prog_catalog_entry_t *b)
{
  if (a->target != b->target)
  {
    return a->target < b->target ? -1 : 1;
  }
  int order = strcmp(a->name, b->name);
  return order ? order : strcmp(a->id, b->id);
}

/* Keeps the PROG_CATALOG_SIZE smallest IDs (strcmp order), so which packages drop out does not depend on FAT order. */
static void progCatalogKeep(prog_catalog_t *catalog, const char *id)
{
  unsigned n = catalog->count;
  if (n == PROG_CATALOG_SIZE)
  {
    catalog->more = true;
    if (strcmp(id, catalog->entries[n - 1].id) >= 0)
    {
      return;
    }
    n--; /* The largest kept ID makes room. */
  }
  unsigned j = n;
  for (; j && strcmp(catalog->entries[j - 1].id, id) > 0; j--)
  {
    strcpy(catalog->entries[j].id, catalog->entries[j - 1].id);
  }
  strcpy(catalog->entries[j].id, id);
  catalog->count = n + 1;
}

FRESULT progCatalogScan(prog_catalog_t *catalog)
{
  memset(catalog, 0, sizeof(*catalog));
  DIR     dir;
  FILINFO info;
  FRESULT e = f_opendir(&dir, "/prog/fw");
  if (e != FR_OK)
  {
    return e;
  }
  while ((e = f_readdir(&dir, &info)) == FR_OK && info.fname[0])
  {
    if (!(info.fattrib & AM_DIR) || !progCatalogIsValidId(info.fname))
    {
      continue;
    }
    char    path[64];
    FILINFO file;
    snprintf(path, sizeof(path), "/prog/fw/%.24s/fw.txt", info.fname);
    if (f_stat(path, &file) != FR_OK || (file.fattrib & AM_DIR))
    {
      continue; /* Not a package; a half-copied one has no fw.txt yet. */
    }
    progCatalogKeep(catalog, info.fname);
  }
  FRESULT closed = f_closedir(&dir);
  if (e != FR_OK || closed != FR_OK)
  {
    catalog->count = 0;
    return e != FR_OK ? e : closed;
  }
  for (unsigned i = 0; i < catalog->count; i++)
  {
    prog_catalog_entry_t *entry = &catalog->entries[i];
    FRESULT               io;
    entry->error  = progPackageLoad(entry->id, &scratch, &io);
    entry->target = scratch.target;
    if (!entry->error)
    {
      entry->error = progCatalogImages(entry->id, &scratch, entry);
    }
    strcpy(entry->name, scratch.manifest.name[0] ? scratch.manifest.name : entry->id);
    strcpy(entry->version, scratch.version);
  }
  for (unsigned i = 1; i < catalog->count; i++)
  {
    prog_catalog_entry_t entry = catalog->entries[i];
    unsigned             j     = i;
    for (; j && progCatalogCompare(&catalog->entries[j - 1], &entry) > 0; j--)
    {
      catalog->entries[j] = catalog->entries[j - 1];
    }
    catalog->entries[j] = entry;
  }
  return FR_OK;
}

unsigned progCatalogFilter(const prog_catalog_t *catalog, prog_target_t target, bool usable_only, uint8_t *index,
                           unsigned size)
{
  unsigned count = 0;
  for (unsigned i = 0; i < catalog->count; i++)
  {
    const prog_catalog_entry_t *entry = &catalog->entries[i];
    if (entry->target == target && (!usable_only || !entry->error))
    {
      if (index && count < size)
      {
        index[count] = (uint8_t)i;
      }
      count++;
    }
  }
  return count;
}
