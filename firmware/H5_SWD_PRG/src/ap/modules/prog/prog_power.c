#include "prog_power.h"
#include "fatfs.h"
#include <string.h>

#define PROG_POWER_DIR  "/prog"
#define PROG_POWER_PATH "/prog/power.cfg"
#define PROG_POWER_TMP  "/prog/power.tmp"
#define PROG_POWER_SIZE 12 /* "PWR1", outputs, mode, 2 reserved, CRC32 of the first 8 bytes. */

static uint32_t progPowerCrc(const uint8_t *data, unsigned size)
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

static void progPowerEncode(const prog_power_config_t *config, uint8_t record[PROG_POWER_SIZE])
{
  memset(record, 0, PROG_POWER_SIZE);
  memcpy(record, "PWR1", 4);
  record[4]    = (config->out_3v3 ? 1U : 0U) | (config->out_5v ? 2U : 0U);
  record[5]    = config->mode;
  uint32_t crc = progPowerCrc(record, 8);
  for (unsigned i = 0; i < 4; i++)
  {
    record[8 + i] = crc >> (8 * i);
  }
}

static bool progPowerDecode(const uint8_t record[PROG_POWER_SIZE], prog_power_config_t *config)
{
  uint32_t crc = (uint32_t)record[8] | (uint32_t)record[9] << 8 | (uint32_t)record[10] << 16 | (uint32_t)record[11] << 24;
  if (memcmp(record, "PWR1", 4) || crc != progPowerCrc(record, 8) || record[4] > 3 || record[5] >= PROG_POWER_MODES)
  {
    return false;
  }
  config->out_3v3 = (record[4] & 1U) != 0;
  config->out_5v  = (record[4] & 2U) != 0;
  config->mode    = record[5];
  return true;
}

bool progPowerLoad(prog_power_config_t *config)
{
  memset(config, 0, sizeof(*config));
  FIL file;
  if (f_open(&file, PROG_POWER_PATH, FA_READ) != FR_OK)
  {
    return false;
  }
  uint8_t record[PROG_POWER_SIZE];
  UINT    got = 0;
  bool    ok =
  f_size(&file) == sizeof(record) && f_read(&file, record, sizeof(record), &got) == FR_OK && got == sizeof(record);
  FRESULT closed = f_close(&file);
  return ok && closed == FR_OK && progPowerDecode(record, config);
}

/* Written beside the old file and renamed over it, so a pulled card keeps the previous settings. */
bool progPowerSave(const prog_power_config_t *config)
{
  uint8_t record[PROG_POWER_SIZE];
  progPowerEncode(config, record);
  FRESULT e = f_mkdir(PROG_POWER_DIR);
  if (e != FR_OK && e != FR_EXIST)
  {
    return false;
  }
  FIL  file;
  UINT written = 0;
  if (f_open(&file, PROG_POWER_TMP, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    return false;
  }
  bool    ok     = f_write(&file, record, sizeof(record), &written) == FR_OK && written == sizeof(record);
  FRESULT synced = f_sync(&file);
  FRESULT closed = f_close(&file);
  if (!ok || synced != FR_OK || closed != FR_OK)
  {
    f_unlink(PROG_POWER_TMP);
    return false;
  }
  e = f_unlink(PROG_POWER_PATH);
  if (e != FR_OK && e != FR_NO_FILE)
  {
    return false;
  }
  return f_rename(PROG_POWER_TMP, PROG_POWER_PATH) == FR_OK;
}
