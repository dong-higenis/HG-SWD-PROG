#ifndef SWD_H_
#define SWD_H_
#include "hw_def.h"
#ifdef __cplusplus
extern "C" {
#endif
bool swdInit(void);
void swdRelease(void);
void swdClockWrite(bool high);
void swdDataOutput(bool enable);
void swdDataWrite(bool high);
bool swdDataRead(void);
bool swdTraceRead(void);
void swdTargetReset(bool asserted);

typedef enum
{
  SWD_OK = 0,
  SWD_ERR_NORESP,
  SWD_ERR_WAIT,
  SWD_ERR_FAULT,
  SWD_ERR_PARITY,
  SWD_ERR_PROTOCOL,
  SWD_ERR_TIMEOUT,
  SWD_ERR_NO_AP,
  SWD_ERR_MISMATCH,
  SWD_ERR_ARGUMENT,
  SWD_ERR_VERIFY,
  SWD_ERR_CORE_STATE,
  SWD_ERR_RESTORE,
  SWD_ERR_PRECONDITION
} swd_err_t;

typedef struct
{
  const char *stage;
  uint32_t dpidr;
  uint32_t ap_idr;
  uint32_t cpuid;
  uint32_t device_id;
  uint32_t ctrl_stat;
  uint32_t last_ack;
  uint32_t wait_count;
  uint8_t ap;
} swd_probe_t;

/* CLI-only, serialized access for now. No target halt/reset, RAM or flash writes.
 * DP/AP control registers are configured to read the target IDs.
 * khz is a timing ceiling (10..500); GPIO/software overhead makes it slower.
 * PB1/PB3/PB4 are released on every return path. */
swd_err_t swdProbeH723(uint32_t khz, swd_probe_t *report);

typedef struct
{
  swd_probe_t link;
  const char *stage;
  uint32_t address;
  uint32_t size;
  uint32_t passes;
  uint32_t fail_addr;
  uint32_t expected;
  uint32_t actual;
  uint32_t dhcsr_before;
  uint32_t dhcsr_after;
  swd_err_t test_status;
  swd_err_t cleanup_status;
  bool core_saved;
  bool was_halted;
  bool core_restored;
  bool backup_saved;
  bool ram_written;
  bool ram_restored;
} swd_ramtest_t;

/* Halt, save/test/restore 64 bytes of H723 DTCM, restore prior run/halt state.
 * No reset/flash operation. A cleanup failure prevents an automatic resume. */
swd_err_t swdRamTestH723(uint32_t khz, swd_ramtest_t *report);

typedef struct
{
  swd_probe_t link;
  const char *stage;
  const char *cleanup_stage;
  swd_err_t test_status;
  swd_err_t cleanup_status;
  uint32_t code_addr;
  uint32_t result;
  uint32_t stop_pc;
  uint32_t dfsr;
  uint32_t itcmcr;
  uint32_t mpu_ctrl;
  uint32_t dhcsr;
  bool started;
  bool was_halted;
  bool code_restored;
  bool regs_restored;
  bool mpu_restored;
  bool core_restored;
} swd_exectest_t;

/* Execute a stackless 7+35 / BX LR / BKPT test in enabled H723 ITCM.
 * Preserve code, modified registers, MPU and original run/halt state.
 * Reject exception/IT state; failed cleanup never intentionally resumes. */
swd_err_t swdExecTestH723(uint32_t khz, swd_exectest_t *report);
const char *swdErrStr(swd_err_t err);
#ifdef __cplusplus
}
#endif
#endif
