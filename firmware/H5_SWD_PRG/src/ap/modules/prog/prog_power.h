#ifndef PROG_POWER_H
#define PROG_POWER_H
#include <stdbool.h>
#include <stdint.h>

/* Target supply on the 7-pin connector: 3V_OUT (PB5) and 5V_OUT (PA9) switch independently. */
typedef enum
{
  PROG_POWER_JOB,    /* Enabled outputs are on only while a job talks to the target. */
  PROG_POWER_ALWAYS, /* Enabled outputs stay on between jobs. */
  PROG_POWER_MODES
} prog_power_mode_t;

typedef struct
{
  bool    out_3v3;
  bool    out_5v;
  uint8_t mode; /* prog_power_mode_t */
} prog_power_config_t;

/* Settings belong to the card, like the Auto bindings; a card without a valid file reads as all off. */
bool progPowerLoad(prog_power_config_t *config);
bool progPowerSave(const prog_power_config_t *config);
#endif
