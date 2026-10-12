#ifndef ESP_PROGRAM_H
#define ESP_PROGRAM_H
#include "esp_project.h"
#include "esp_loader.h"
#include "esp/esp_port.h"

typedef struct
{
  void *context;
  bool (*read)(void *context, unsigned image, uint32_t offset, void *data, uint32_t size);
  bool (*progress)(void *context, const char *stage, uint32_t done, uint32_t total);
} esp_program_input_t;

typedef struct
{
  const char        *stage;
  const char        *error;
  esp_loader_error_t code;
  target_chip_t      detected;
  uint32_t           flash_size;
  uint32_t           address;
  uint32_t           written;
  uint32_t           verified;
  uint32_t           elapsed_ms;
  esp_port_stats_t   port;
  uint32_t           link_test_ms;
  unsigned           rate_fallbacks;
  bool               stub;
  bool               erase_complete;
  bool               started;
} esp_program_result_t;

/* Caller owns the shared CLI/hardware lock. All SD files must pass preflight first. */
bool        espProgramRun(const esp_project_t *project, const esp_program_input_t *input, esp_program_result_t *result);
bool        espProgramErase(esp_project_chip_t chip, const esp_program_input_t *input, esp_program_result_t *result);
bool        espProgramProbe(esp_program_result_t *result);
const char *espProgramError(esp_loader_error_t code);
#endif
