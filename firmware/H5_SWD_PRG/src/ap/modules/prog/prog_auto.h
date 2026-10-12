#ifndef PROG_AUTO_H
#define PROG_AUTO_H
#include <stdbool.h>
#include <stdint.h>

#define PROG_AUTO_TARGETS     4
#define PROG_AUTO_ID_SIZE     25
#define PROG_AUTO_RECORD_SIZE 112

typedef struct
{
  uint32_t sequence;
  char     project[PROG_AUTO_TARGETS][PROG_AUTO_ID_SIZE];
} prog_auto_config_t;

bool progAutoDecode(const uint8_t record[PROG_AUTO_RECORD_SIZE], prog_auto_config_t *config);
void progAutoEncode(const prog_auto_config_t *config, uint8_t record[PROG_AUTO_RECORD_SIZE]);
bool progAutoNewer(uint32_t left, uint32_t right);
#endif
