#include "hub.h"
#include "net.h"
#include "fatfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HUB_CHUNK NET_HTTP_READ_MAX

static char hub_error[40]; /* Reasons built at run time (HTTP code, FatFs code). */

void hubJsonBegin(hub_json_t *json, hub_list_t *list)
{
  memset(json, 0, sizeof(*json));
  memset(list, 0, sizeof(*list));
  json->list = list;
}

static void hubJsonCopy(char *out, size_t size, const char *text)
{
  snprintf(out, size, "%s", text);
}

static void hubJsonEmit(hub_json_t *json)
{
  hub_project_t *p = &json->current;
  if (!progCatalogIsValidId(p->id))
  {
    return; /* The Hub's CI checks IDs; anything else is not a project this firmware can store. */
  }
  if (!p->name[0])
  {
    hubJsonCopy(p->name, sizeof(p->name), p->id);
  }
  p->target = progTargetFromChip(p->chip);
  if (json->list->count == HUB_PROJECTS)
  {
    json->list->more = true;
    return;
  }
  json->list->projects[json->list->count++] = *p;
}

/* A string just ended at the current depth: a key, or a value of the key before it. */
static void hubJsonString(hub_json_t *json)
{
  uint8_t d          = json->depth;
  json->text[json->text_len] = 0;
  if (json->expect_key[d])
  {
    hubJsonCopy(json->key[d], sizeof(json->key[d]), json->text);
    return;
  }
  if (!json->in_project || d != 3)
  {
    return;
  }
  hub_project_t *p = &json->current;
  if (!strcmp(json->key[d], "id"))
  {
    hubJsonCopy(p->id, sizeof(p->id), json->text);
  }
  else if (!strcmp(json->key[d], "name"))
  {
    hubJsonCopy(p->name, sizeof(p->name), json->text);
  }
  else if (!strcmp(json->key[d], "chip"))
  {
    hubJsonCopy(p->chip, sizeof(p->chip), json->text);
  }
  else if (!strcmp(json->key[d], "latest"))
  {
    hubJsonCopy(p->version, sizeof(p->version), json->text);
  }
}

static void hubJsonText(hub_json_t *json, char c)
{
  if (json->text_len < sizeof(json->text) - 1)
  {
    json->text[json->text_len++] = c;
  }
}

void hubJsonFeed(hub_json_t *json, const uint8_t *data, uint32_t size)
{
  for (uint32_t i = 0; i < size && !json->error; i++)
  {
    char c = (char)data[i];
    if (json->in_string)
    {
      if (json->unicode)
      {
        if (!--json->unicode)
        {
          hubJsonText(json, '?'); /* Non-ASCII; the fields the device uses are ASCII. */
        }
      }
      else if (json->escape)
      {
        json->escape = false;
        if (c == 'u')
        {
          json->unicode = 4;
        }
        else
        {
          hubJsonText(json, c == '"' || c == '\\' || c == '/' ? c : ' ');
        }
      }
      else if (c == '\\')
      {
        json->escape = true;
      }
      else if (c == '"')
      {
        json->in_string = false;
        hubJsonString(json);
      }
      else
      {
        hubJsonText(json, (unsigned char)c < 32 || (unsigned char)c > 126 ? '?' : c);
      }
      continue;
    }
    switch (c)
    {
      case '"':
        json->in_string = true;
        json->text_len  = 0;
        break;
      case '{':
      case '[':
        if (json->depth == 7)
        {
          json->error = true;
          break;
        }
        json->depth++;
        json->object[json->depth]     = c == '{';
        json->expect_key[json->depth] = c == '{';
        json->key[json->depth][0]     = 0;
        if (c == '[' && json->depth == 2 && !strcmp(json->key[1], "projects"))
        {
          json->projects = true;
        }
        if (c == '{' && json->depth == 3 && json->projects)
        {
          json->in_project = true;
          memset(&json->current, 0, sizeof(json->current));
        }
        break;
      case '}':
      case ']':
        if (!json->depth)
        {
          json->error = true;
          break;
        }
        if (json->depth == 3 && json->in_project)
        {
          hubJsonEmit(json);
          json->in_project = false;
        }
        if (json->depth == 2 && json->projects)
        {
          json->projects = false;
        }
        json->depth--;
        break;
      case ':':
        json->expect_key[json->depth] = false;
        break;
      case ',':
        json->expect_key[json->depth] = json->object[json->depth]; /* In an object the next string is a key. */
        break;
      default:
        break; /* Numbers, true/false/null and white space. */
    }
  }
}

static char *hubTrim(char *s)
{
  s        += strspn(s, " \t\r");
  size_t n  = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
  {
    s[--n] = 0;
  }
  return s;
}

/* Basename or relative path from the Hub: no "..", no backslash, nothing a FAT name cannot hold. */
static bool hubIsSafe(const char *s, bool slash)
{
  if (!*s || strstr(s, ".."))
  {
    return false;
  }
  for (; *s; s++)
  {
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' ||
          *s == '_' || *s == '-' || (slash && *s == '/')))
    {
      return false;
    }
  }
  return true;
}

const char *hubLatestParse(char *text, hub_latest_t *out)
{
  memset(out, 0, sizeof(*out));
  bool schema = false;
  for (char *line = text; line && *line;)
  {
    char *next = strchr(line, '\n');
    if (next)
    {
      *next++ = 0;
    }
    char *key = hubTrim(line);
    line      = next;
    char *v   = strchr(key, '=');
    if (!*key || *key == '#' || !v)
    {
      continue;
    }
    *v++ = 0;
    key  = hubTrim(key);
    v    = hubTrim(v);
    if (!strcmp(key, "schema"))
    {
      if (strcmp(v, "1"))
      {
        return "latest.txt schema";
      }
      schema = true;
    }
    else if (!strcmp(key, "id"))
    {
      hubJsonCopy(out->id, sizeof(out->id), v);
    }
    else if (!strcmp(key, "version"))
    {
      hubJsonCopy(out->version, sizeof(out->version), v);
    }
    else if (!strcmp(key, "base"))
    {
      if (strlen(v) >= sizeof(out->base) || !hubIsSafe(v, true))
      {
        return "latest.txt base";
      }
      strcpy(out->base, v);
    }
    else if (!strcmp(key, "file"))
    {
      char *name = v, *size = strchr(name, ','), *crc = size ? strchr(size + 1, ',') : NULL;
      if (!size || !crc || out->count == HUB_FILES)
      {
        return "latest.txt file";
      }
      *size++ = 0;
      *crc++  = 0;
      char *end;
      hub_file_t *f = &out->files[out->count];
      name          = hubTrim(name);
      if (strlen(name) >= sizeof(f->name) || !hubIsSafe(name, false))
      {
        return "latest.txt name";
      }
      strcpy(f->name, name);
      f->size  = (uint32_t)strtoul(hubTrim(size), &end, 10);
      bool bad = *end != 0;
      f->crc32 = (uint32_t)strtoul(hubTrim(crc), &end, 16);
      if (bad || (*end && *end != ','))
      {
        return "latest.txt number";
      }
      out->count++;
    }
  }
  if (!schema || !out->count || !out->base[0] || !progCatalogIsValidId(out->id))
  {
    return "latest.txt incomplete";
  }
  if (strcmp(out->files[out->count - 1].name, "fw.txt"))
  {
    return "latest.txt order"; /* fw.txt last keeps half-copied packages invisible. */
  }
  return NULL;
}

static uint32_t hubCrc(uint32_t crc, const uint8_t *data, uint32_t size)
{
  crc = ~crc;
  for (uint32_t n = 0; n < size; n++)
  {
    crc ^= data[n];
    for (unsigned bit = 0; bit < 8; bit++)
    {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
    }
  }
  return ~crc;
}

static const char *hubNetError(uint8_t status)
{
  return netStatusText(status);
}

static const char *hubSdError(FRESULT e)
{
  snprintf(hub_error, sizeof(hub_error), "SD error %u", e);
  return hub_error;
}

/* GET with the status line checked; the body is then read with netHttpRead. */
static const char *hubOpen(const char *url, uint32_t *length)
{
  uint16_t code   = 0;
  uint8_t  status = netHttpGet(url, &code, length);
  if (status != NET_ST_OK)
  {
    return hubNetError(status);
  }
  if (code != 200)
  {
    netHttpClose();
    snprintf(hub_error, sizeof(hub_error), "HTTP %u", code);
    return hub_error;
  }
  return NULL;
}

const char *hubFetchList(hub_list_t *list, hub_progress_t progress, void *context)
{
  static hub_json_t json;
  static uint8_t    chunk[HUB_CHUNK];
  uint32_t          length = 0, done = 0;
  hubJsonBegin(&json, list);
  if (!progress(context, "HUB LIST", 0, 1))
  {
    return "Stopped";
  }
  const char *error = hubOpen(HUB_URL "manifest.json", &length);
  while (!error)
  {
    uint16_t got    = 0;
    uint8_t  status = netHttpRead(chunk, sizeof(chunk), &got);
    if (status != NET_ST_OK)
    {
      error = hubNetError(status);
    }
    else if (!got)
    {
      break;
    }
    else
    {
      hubJsonFeed(&json, chunk, got);
      done += got;
      if (!progress(context, "HUB LIST", length == UINT32_MAX ? 0 : done, length == UINT32_MAX ? 1 : length))
      {
        error = "Stopped";
      }
    }
  }
  netHttpClose();
  if (!error && (json.error || json.depth))
  {
    error = "Bad manifest";
  }
  return error;
}

static FRESULT hubMkdir(const char *path)
{
  FRESULT e = f_mkdir(path);
  return e == FR_EXIST ? FR_OK : e;
}

static const char *hubDownload(const char *url, const char *part, const hub_file_t *file, uint32_t *done,
                               uint32_t total, hub_progress_t progress, void *context)
{
  static uint8_t chunk[HUB_CHUNK];
  uint32_t       length = 0, size = 0, crc = 0;
  const char    *error  = hubOpen(url, &length);
  if (error)
  {
    return error;
  }
  if (length != UINT32_MAX && length != file->size)
  {
    netHttpClose();
    return "Size mismatch";
  }
  FIL     out;
  FRESULT e = f_open(&out, part, FA_WRITE | FA_CREATE_ALWAYS);
  if (e != FR_OK)
  {
    netHttpClose();
    return hubSdError(e);
  }
  while (!error)
  {
    uint16_t got    = 0;
    uint8_t  status = netHttpRead(chunk, sizeof(chunk), &got);
    UINT     written = 0;
    if (status != NET_ST_OK)
    {
      error = hubNetError(status);
    }
    else if (!got)
    {
      break;
    }
    else if (size + got > file->size)
    {
      error = "Size mismatch";
    }
    else if ((e = f_write(&out, chunk, got, &written)) != FR_OK || written != got)
    {
      error = hubSdError(e != FR_OK ? e : FR_DENIED); /* A full card writes short. */
    }
    else
    {
      crc     = hubCrc(crc, chunk, got);
      size   += got;
      *done  += got;
      if (!progress(context, file->name, *done, total))
      {
        error = "Stopped";
      }
    }
  }
  netHttpClose();
  e = f_close(&out);
  if (!error && e != FR_OK)
  {
    error = hubSdError(e);
  }
  if (!error && size != file->size)
  {
    error = "Size mismatch";
  }
  if (!error && crc != file->crc32)
  {
    error = "CRC mismatch";
  }
  if (error)
  {
    f_unlink(part);
  }
  return error;
}

const char *hubInstall(const hub_project_t *project, hub_progress_t progress, void *context)
{
  static hub_latest_t latest;
  static char         text[2049];
  char                url[256], path[96], part[112];
  uint32_t            length = 0, size = 0;
  if (!progress(context, "HUB INFO", 0, 1))
  {
    return "Stopped";
  }
  snprintf(url, sizeof(url), HUB_URL "fw/%s/latest.txt", project->id);
  const char *error = hubOpen(url, &length);
  while (!error)
  {
    uint16_t got    = 0;
    uint32_t room   = sizeof(text) - 1 - size;
    uint8_t  status = netHttpRead(text + size, (uint16_t)(room < HUB_CHUNK ? room : HUB_CHUNK), &got);
    if (status != NET_ST_OK)
    {
      error = hubNetError(status);
    }
    else if (!got)
    {
      break;
    }
    else if ((size += got) == sizeof(text) - 1)
    {
      error = "latest.txt size";
    }
  }
  netHttpClose();
  if (error)
  {
    return error;
  }
  text[size] = 0;
  if ((error = hubLatestParse(text, &latest)))
  {
    return error;
  }
  if (strcmp(latest.id, project->id))
  {
    return "latest.txt id";
  }
  uint32_t total = 0, done = 0;
  for (unsigned i = 0; i < latest.count; i++)
  {
    total += latest.files[i].size;
  }
  FRESULT e;
  snprintf(path, sizeof(path), "/prog/fw/%s", project->id);
  if ((e = hubMkdir("/prog")) != FR_OK || (e = hubMkdir("/prog/fw")) != FR_OK || (e = hubMkdir(path)) != FR_OK)
  {
    return hubSdError(e);
  }
  /* Without fw.txt the folder is not a package until the new one is complete. */
  snprintf(path, sizeof(path), "/prog/fw/%s/fw.txt", project->id);
  e = f_unlink(path);
  if (e != FR_OK && e != FR_NO_FILE)
  {
    return hubSdError(e);
  }
  for (unsigned i = 0; i < latest.count && !error; i++)
  {
    const hub_file_t *file = &latest.files[i];
    snprintf(url, sizeof(url), HUB_URL "%s%s", latest.base, file->name);
    snprintf(path, sizeof(path), "/prog/fw/%s/%s", project->id, file->name);
    snprintf(part, sizeof(part), "%s.part", path);
    error = hubDownload(url, part, file, &done, total, progress, context);
    if (!error)
    {
      e = f_unlink(path);
      if ((e != FR_OK && e != FR_NO_FILE) || (e = f_rename(part, path)) != FR_OK)
      {
        f_unlink(part);
        error = hubSdError(e);
      }
    }
  }
  return error;
}
