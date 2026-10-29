#include "prog_target.h"
#include <string.h>

static const prog_target_info_t targets[PROG_TARGETS] = {
  [PROG_TARGET_H723]    = {"H723", "stm32h723", "STM32H72x/STM32H73x", false, ESP_PROJECT_32, false},
  [PROG_TARGET_ESP32]   = {"ESP32", "esp32", "ESP32", true, ESP_PROJECT_32, false},
  [PROG_TARGET_ESP32S3] = {"ESP32-S3", "esp32s3", "ESP32-S3", true, ESP_PROJECT_S3, false},
  [PROG_TARGET_ESP32C3] = {"ESP32-C3", "esp32c3", "ESP32-C3", true, ESP_PROJECT_C3, false},
};

const prog_target_info_t *progTargetInfo(prog_target_t target)
{
  return target < PROG_TARGETS ? &targets[target] : NULL;
}

const char *progTargetLabel(prog_target_t target)
{
  return target < PROG_TARGETS ? targets[target].label : "Unknown";
}

/* Packages without release.txt: the one chip of a device group that is not shared_device. */
prog_target_t progTargetFromDevice(const char *device)
{
  for (unsigned target = 0; target < PROG_TARGETS; target++)
  {
    if (!targets[target].shared && !strcmp(device, targets[target].device))
    {
      return (prog_target_t)target;
    }
  }
  return PROG_TARGET_NONE;
}

prog_target_t progTargetFromChip(const char *chip)
{
  for (unsigned target = 0; target < PROG_TARGETS; target++)
  {
    if (!strcmp(chip, targets[target].chip))
    {
      return (prog_target_t)target;
    }
  }
  return PROG_TARGET_NONE;
}

prog_target_t progTargetFromEsp(esp_project_chip_t chip)
{
  for (unsigned target = 0; target < PROG_TARGETS; target++)
  {
    if (targets[target].esp && targets[target].esp_chip == chip)
    {
      return (prog_target_t)target;
    }
  }
  return PROG_TARGET_NONE;
}
