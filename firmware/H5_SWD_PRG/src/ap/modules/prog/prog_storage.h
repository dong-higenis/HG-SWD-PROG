#ifndef PROG_STORAGE_H
#define PROG_STORAGE_H
#include "fatfs.h"

typedef struct
{
  void *context;
  /* stage, entries done, entries total; done stays 0 while counting. Return false to stop. */
  bool (*progress)(void *context, const char *stage, uint32_t done, uint32_t total);
} prog_storage_input_t;

typedef struct
{
  uint32_t removed; /* Files and folders deleted. */
  FRESULT  io;      /* FatFs code of the failing call, FR_OK otherwise. */
} prog_storage_result_t;

/* Deletes every file and folder on the mounted card, keeping the file system itself (not a format).
 * Returns NULL on success or a short reason; a stop leaves the remaining entries in place. */
const char *progStorageWipe(const prog_storage_input_t *input, prog_storage_result_t *result);
#endif
