#ifndef PROG_TARGET_H
#define PROG_TARGET_H
#include "esp_project.h"
#include <stdbool.h>

/* Chips the programmer can write, in the order of the Auto record (prog_auto.c); never reorder. */
typedef enum
{
  PROG_TARGET_H723,
  PROG_TARGET_ESP32,
  PROG_TARGET_ESP32S3,
  PROG_TARGET_ESP32C3,
  PROG_TARGETS,
  PROG_TARGET_NONE = PROG_TARGETS /* fw.txt without a known device= line. */
} prog_target_t;

/* Mirrors HG-SWD-FW-HUB registry/chips/<chip>.toml, so SD packages group the way the Hub lists them. */
typedef struct
{
  const char        *label;  /* OLED, at most 8 characters. */
  const char        *chip;   /* Hub registry chip ID (latest.txt chip=). */
  const char        *device; /* fw.txt device=. */
  bool               esp;
  esp_project_chip_t esp_chip;
  bool               shared; /* Hub shared_device: recognised only through release.txt chip=, never by device= alone. */
} prog_target_info_t;

const prog_target_info_t *progTargetInfo(prog_target_t target);
const char               *progTargetLabel(prog_target_t target);
prog_target_t             progTargetFromDevice(const char *device);
prog_target_t             progTargetFromChip(const char *chip);
prog_target_t             progTargetFromEsp(esp_project_chip_t chip);
#endif
