#ifndef SWD_PROGRAM_H
#define SWD_PROGRAM_H
#include "swd.h"

typedef struct
{
  const uint32_t *code; /* Inspected CubeCLT 1.20.0 0x483.stldr code, 2020 bytes. */
  uint32_t        code_size;
  uint32_t        image_size;
  uint32_t        image_crc;
  void           *context;
  bool (*read)(void *context, uint32_t offset, void *data, uint32_t size);
  /* Return false to request cancellation, applied between loader calls. */
  bool (*progress)(void *context, const char *stage, uint32_t done, uint32_t total);
} swd_program_input_t;

typedef struct
{
  swd_probe_t link;
  const char *stage;
  swd_err_t   error;
  uint32_t    address;
  uint32_t    flash_sr;
  uint32_t    pc;
  uint32_t    dfsr;
  uint32_t    result;
  uint32_t    written;
  uint32_t    verified;
  uint32_t    elapsed_ms;
  uint32_t    clock_khz;
  uint32_t    link_test_ms;
  uint32_t    erase_ms;
  uint32_t    write_ms;
  uint32_t    verify_ms;
  /* Saved before recovery reconnect changes the live DP/AP report. */
  const char *failed_io;
  uint32_t    failed_io_address;
  uint32_t    failed_ack;
  uint32_t    failed_ctrl;
  bool        flash_sr_valid;
  bool        flash_status_error;
  bool        erase_complete; /* Entire 1MiB verified blank, flash locked, core halted. */
  bool        erased;
  bool        reset_attempted;
  bool        halted_on_error;
  bool        cancelled;
  bool        started;
} swd_program_result_t;

/* Destructive operation, caller must own the hardware/CLI resource lock.
 * Checks H723 family + 1MiB, resets/halt, erases covered sectors, writes and
 * compares the entire image, then resets/runs. Failure never intentionally runs.
 * Uses static transfer buffers and is not reentrant. */
swd_err_t swdProgramH723(const swd_program_input_t *input, swd_program_result_t *result);
/* Same loader/context/progress fields as programming; image/read fields ignored.
 * Erases all 8 sectors, verifies every word is FF, locks flash and stays halted. */
swd_err_t swdEraseH723(const swd_program_input_t *input, swd_program_result_t *result);
#endif
