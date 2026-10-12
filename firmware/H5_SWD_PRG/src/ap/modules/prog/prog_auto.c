#include "prog_auto.h"
#include <string.h>

static uint32_t progAutoCrc(const uint8_t *data, unsigned size)
{
  uint32_t crc = UINT32_MAX;
  for (unsigned n = 0; n < size; n++)
  {
    crc ^= data[n];
    for (unsigned bit = 0; bit < 8; bit++)
    {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
    }
  }
  return ~crc;
}

static uint32_t progAutoRead32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void progAutoWrite32(uint8_t *p, uint32_t value)
{
  for (unsigned i = 0; i < 4; i++)
  {
    p[i] = value >> (8 * i);
  }
}

bool progAutoNewer(uint32_t left, uint32_t right)
{
  uint32_t distance = left - right;
  return distance && distance < 0x80000000U;
}

bool progAutoDecode(const uint8_t record[PROG_AUTO_RECORD_SIZE], prog_auto_config_t *config)
{
  if (memcmp(record, "AUT1", 4) || progAutoRead32(record + 108) != progAutoCrc(record, 108))
  {
    return false;
  }
  prog_auto_config_t decoded = {0};
  decoded.sequence           = progAutoRead32(record + 4);
  for (unsigned target = 0; target < PROG_AUTO_TARGETS; target++)
  {
    const uint8_t *id         = record + 8 + target * PROG_AUTO_ID_SIZE;
    bool           terminated = false;
    for (unsigned i = 0; i < PROG_AUTO_ID_SIZE; i++)
    {
      uint8_t c = id[i];
      if (!c)
      {
        terminated = true;
      }
      else if (terminated ||
               !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      {
        return false;
      }
    }
    if (!terminated)
    {
      return false;
    }
    memcpy(decoded.project[target], id, PROG_AUTO_ID_SIZE);
  }
  *config = decoded;
  return true;
}

void progAutoEncode(const prog_auto_config_t *config, uint8_t record[PROG_AUTO_RECORD_SIZE])
{
  memset(record, 0, PROG_AUTO_RECORD_SIZE);
  memcpy(record, "AUT1", 4);
  progAutoWrite32(record + 4, config->sequence);
  for (unsigned i = 0; i < PROG_AUTO_TARGETS; i++)
  {
    memcpy(record + 8 + i * PROG_AUTO_ID_SIZE, config->project[i], strlen(config->project[i]));
  }
  progAutoWrite32(record + 108, progAutoCrc(record, 108));
}
