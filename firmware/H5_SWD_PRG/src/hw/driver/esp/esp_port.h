#ifndef ESP_PORT_H
#define ESP_PORT_H
#include "esp_loader_io.h"
#include <stdint.h>

typedef struct
{
  uint32_t baud;
  uint32_t received;
  uint32_t framing;
  uint32_t overflow;
  uint32_t late;
} esp_port_stats_t;

esp_loader_port_t *espPortGet(void);
void               espPortClose(void);
void               espPortStats(esp_port_stats_t *out);
#endif
