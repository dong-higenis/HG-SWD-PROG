#ifndef ESP_PROJECT_H
#define ESP_PROJECT_H
#include <stdbool.h>
#include <stdint.h>

#define ESP_PROJECT_IMAGES 8
#define ESP_PROJECT_LIMIT  0x01000000U

typedef enum
{
  ESP_PROJECT_32,
  ESP_PROJECT_S3,
  ESP_PROJECT_C3
} esp_project_chip_t;

typedef struct
{
  char     file[48];
  uint32_t address;
  uint32_t size;
  uint32_t crc32;
} esp_project_image_t;

typedef struct
{
  char                name[48];
  esp_project_chip_t  chip;
  uint32_t            baud; /* 0 = automatic; otherwise the maximum UART rate. */
  uint32_t            flash_size;
  uint32_t            total;
  unsigned            count;
  esp_project_image_t images[ESP_PROJECT_IMAGES];
} esp_project_t;

bool        espProjectIsManifest(const char *text);
const char *espProjectParse(char *text, esp_project_t *out);
const char *espProjectChipName(esp_project_chip_t chip);
uint16_t    espProjectChipId(esp_project_chip_t chip);
#endif
