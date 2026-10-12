#include "esp_project.h"
#include <stddef.h>
#include <string.h>

static char *espProjectTrim(char *s)
{
  while (*s == ' ' || *s == '\t' || *s == '\r')
  {
    s++;
  }
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
  {
    s[--n] = 0;
  }
  return s;
}

static bool espProjectHex(const char *s, uint32_t *out)
{
  if (s[0] != '0' || s[1] != 'x' || !s[2])
  {
    return false;
  }
  uint32_t v     = 0;
  unsigned count = 0;
  for (s += 2; *s; s++)
  {
    unsigned d = *s >= '0' && *s <= '9'     ? (unsigned)(*s - '0')
                 : *s >= 'a' && *s <= 'f'   ? (unsigned)(*s - 'a' + 10)
                   : *s >= 'A' && *s <= 'F' ? (unsigned)(*s - 'A' + 10)
                                            : 16;
    if (d > 15 || ++count > 8)
    {
      return false;
    }
    v = v * 16 + d;
  }
  *out = v;
  return true;
}

bool espProjectIsManifest(const char *text)
{
  /* Dispatch only. The full parser below rejects duplicates and malformed fields. */
  for (const char *s = text; *s;)
  {
    while (*s == ' ' || *s == '\t')
    {
      s++;
    }
    if (!strncmp(s, "device", 6))
    {
      const char *v = s + 6;
      while (*v == ' ' || *v == '\t')
      {
        v++;
      }
      if (*v++ == '=')
      {
        while (*v == ' ' || *v == '\t')
        {
          v++;
        }
        if (!strncmp(v, "ESP", 3))
        {
          return true;
        }
      }
    }
    s = strchr(s, '\n');
    if (!s)
    {
      break;
    }
    s++;
  }
  return false;
}

const char *espProjectChipName(esp_project_chip_t chip)
{
  switch (chip)
  {
    case ESP_PROJECT_32:
      return "ESP32";
    case ESP_PROJECT_S3:
      return "ESP32-S3";
    case ESP_PROJECT_C3:
      return "ESP32-C3";
    default:
      return "UNKNOWN";
  }
}

uint16_t espProjectChipId(esp_project_chip_t chip)
{
  return chip == ESP_PROJECT_32 ? 0 : chip == ESP_PROJECT_S3 ? 9 : chip == ESP_PROJECT_C3 ? 5 : 0xFFFF;
}

const char *espProjectParse(char *text, esp_project_t *out)
{
  memset(out, 0, sizeof(*out));
  unsigned seen = 0;
  for (char *line = text; line && *line;)
  {
    char *next = strchr(line, '\n');
    if (next)
    {
      *next++ = 0;
    }
    char *key = espProjectTrim(line);
    line      = next;
    if (!*key || *key == '#')
    {
      continue;
    }
    char *v = strchr(key, '=');
    if (!v)
    {
      return "ESP: expected key=value";
    }
    *v++         = 0;
    key          = espProjectTrim(key);
    v            = espProjectTrim(v);
    unsigned bit = 0;
    if (!strcmp(key, "name"))
    {
      bit = 1;
      if (!*v || strlen(v) >= sizeof(out->name))
      {
        return "ESP: invalid name";
      }
      for (const char *s = v; *s; s++)
      {
        if ((unsigned char)*s < 32 || (unsigned char)*s > 126)
        {
          return "ESP: ASCII name required";
        }
      }
      strcpy(out->name, v);
    }
    else if (!strcmp(key, "device"))
    {
      bit = 2;
      if (!strcmp(v, "ESP32"))
      {
        out->chip = ESP_PROJECT_32;
      }
      else if (!strcmp(v, "ESP32-S3"))
      {
        out->chip = ESP_PROJECT_S3;
      }
      else if (!strcmp(v, "ESP32-C3"))
      {
        out->chip = ESP_PROJECT_C3;
      }
      else
      {
        return "ESP: unsupported chip";
      }
    }
    else if (!strcmp(key, "baud"))
    {
      bit = 4;
      if (!strcmp(v, "auto"))
      {
        out->baud = 0;
      }
      else if (!strcmp(v, "115200"))
      {
        out->baud = 115200;
      }
      else if (!strcmp(v, "230400"))
      {
        out->baud = 230400;
      }
      else if (!strcmp(v, "460800"))
      {
        out->baud = 460800;
      }
      else if (!strcmp(v, "921600"))
      {
        out->baud = 921600;
      }
      else
      {
        return "ESP: invalid baud";
      }
    }
    else if (!strcmp(key, "flash_size"))
    {
      bit = 8;
      if (!espProjectHex(v, &out->flash_size) || out->flash_size < 0x100000 || out->flash_size > ESP_PROJECT_LIMIT ||
          (out->flash_size & (out->flash_size - 1)))
      {
        return "ESP: invalid flash size";
      }
    }
    else if (!strcmp(key, "image"))
    {
      if (out->count == ESP_PROJECT_IMAGES)
      {
        return "ESP: too many images";
      }
      char *parts[4] = {v};
      for (unsigned i = 1; i < 4; i++)
      {
        char *sep = strchr(parts[i - 1], ',');
        if (!sep)
        {
          return "ESP: image=file,address,size,crc";
        }
        *sep++   = 0;
        parts[i] = sep;
      }
      for (unsigned i = 0; i < 4; i++)
      {
        parts[i] = espProjectTrim(parts[i]);
      }
      esp_project_image_t *img = &out->images[out->count];
      if (!*parts[0] || strlen(parts[0]) >= sizeof(img->file) || strstr(parts[0], ".."))
      {
        return "ESP: invalid filename";
      }
      for (const char *s = parts[0]; *s; s++)
      {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' ||
              *s == '_' || *s == '.'))
        {
          return "ESP: basename required";
        }
      }
      strcpy(img->file, parts[0]);
      if (!espProjectHex(parts[1], &img->address) || !espProjectHex(parts[2], &img->size) ||
          !espProjectHex(parts[3], &img->crc32))
      {
        return "ESP: invalid image number";
      }
      if (!img->size || img->address >= ESP_PROJECT_LIMIT || (img->address & 4095) ||
          img->size > ESP_PROJECT_LIMIT - img->address)
      {
        return "ESP: invalid flash range";
      }
      uint32_t end = img->address + ((img->size + 4095) & ~4095U);
      for (unsigned i = 0; i < out->count; i++)
      {
        const esp_project_image_t *other     = &out->images[i];
        uint32_t                   other_end = other->address + ((other->size + 4095) & ~4095U);
        if (img->address < other_end && other->address < end)
        {
          return "ESP: overlapping erase ranges";
        }
        if (!strcmp(img->file, other->file))
        {
          return "ESP: duplicate image";
        }
      }
      out->count++;
      out->total += img->size;
    }
    else
    {
      return "ESP: unknown field";
    }
    if (seen & bit)
    {
      return "ESP: duplicate field";
    }
    seen |= bit;
  }
  if (seen != 15 || !out->count)
  {
    return "ESP: missing field/image";
  }
  for (unsigned i = 0; i < out->count; i++)
  {
    const esp_project_image_t *img = &out->images[i];
    if (img->address >= out->flash_size || img->size > out->flash_size - img->address)
    {
      return "ESP: image exceeds flash size";
    }
  }
  return NULL;
}
