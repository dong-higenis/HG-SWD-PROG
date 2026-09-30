/* Stackless H723 runner bring-up. ITCM avoids debugger/I-cache coherency
 * issues (Arm DDI0489F, memory system and debug interface sections).
 * The real flash loader will need a separate stack, cache and reset policy. */
#include "swd_dap.h"
#include "swd_transport.h"
#include <string.h>

#ifdef _USE_HW_SWD
#define CODE_ADDR      0x00000100UL
#define DHCSR          0xE000EDF0UL
#define DCRSR          0xE000EDF4UL
#define DCRDR          0xE000EDF8UL
#define DFSR           0xE000ED30UL
#define ITCMCR         0xE000EF90UL
#define MPU_CTRL       0xE000ED94UL
#define DEBUG_KEY      0xA05F0000UL
#define C_DEBUGEN      1U
#define C_HALT         2U
#define C_STEP         4U
#define C_MASKINTS     8U
#define S_REGRDY       (1UL << 16)
#define S_HALT         (1UL << 17)
#define S_LOCKUP       (1UL << 19)
#define S_RESET_ST     (1UL << 25)
#define XPSR_THUMB     (1UL << 24)
#define XPSR_IPSR      0x1FFUL
#define XPSR_IT_ICI    0x0600FC00UL

/* adds r0,r0,r1; bx lr; bkpt #0; b .
 * Uses no stack/data memory/FPU or general registers other than r0/r1/lr. */
static const uint32_t code_words[2] = {0x47701840UL, 0xE7FEBE00UL};
static const uint8_t saved_regs[5] = {0, 1, 14, 15, 16};

typedef struct
{
  swd_exectest_t *r;
  uint32_t dhcsr_before;
  uint32_t registers[5];
  uint32_t sp_before;
  uint32_t code_before[2];
  bool halted_by_us;
  bool reset_seen;
} exec_ctx_t;

static swd_err_t readMem(exec_ctx_t *c, uint32_t addr, uint32_t *value)
{
  return swdMemRead32(&c->r->link, addr, value);
}

static swd_err_t writeMem(exec_ctx_t *c, uint32_t addr, uint32_t value)
{
  return swdMemWrite32(&c->r->link, addr, value);
}

static swd_err_t state(exec_ctx_t *c, bool require_halt)
{
  swd_err_t err = readMem(c, DHCSR, &c->r->dhcsr);
  if (err != SWD_OK) return err;
  if (c->r->dhcsr & S_RESET_ST) c->reset_seen = true;
  if (c->reset_seen || (c->r->dhcsr & S_LOCKUP)) return SWD_ERR_CORE_STATE;
  if (require_halt && !(c->r->dhcsr & S_HALT)) return SWD_ERR_CORE_STATE;
  return SWD_OK;
}

static swd_err_t waitState(exec_ctx_t *c, uint32_t mask, uint32_t timeout)
{
  uint32_t start = millis();
  do
  {
    swd_err_t err = state(c, mask == S_REGRDY);
    if (err != SWD_OK) return err;
    if (c->r->dhcsr & mask) return SWD_OK;
  } while ((uint32_t)(millis() - start) < timeout);
  return SWD_ERR_TIMEOUT;
}

static swd_err_t regRead(exec_ctx_t *c, uint8_t reg, uint32_t *value)
{
  swd_err_t err = state(c, true);
  if (err != SWD_OK) return err;
  err = writeMem(c, DCRSR, reg);
  if (err != SWD_OK) return err;
  err = waitState(c, S_REGRDY, 100);
  if (err != SWD_OK) return err;
  return readMem(c, DCRDR, value);
}

static swd_err_t regWrite(exec_ctx_t *c, uint8_t reg, uint32_t value)
{
  swd_err_t err = state(c, true);
  if (err != SWD_OK) return err;
  err = writeMem(c, DCRDR, value);
  if (err != SWD_OK) return err;
  err = writeMem(c, DCRSR, (1UL << 16) | reg);
  if (err != SWD_OK) return err;
  return waitState(c, S_REGRDY, 100);
}

static swd_err_t writeChecked(exec_ctx_t *c, uint32_t addr, uint32_t value)
{
  swd_err_t err = state(c, true);
  if (err != SWD_OK) return err;
  err = writeMem(c, addr, value);
  if (err != SWD_OK) return err;
  uint32_t actual;
  err = readMem(c, addr, &actual);
  if (err != SWD_OK) return err;
  return actual == value ? SWD_OK : SWD_ERR_VERIFY;
}

static swd_err_t prepare(exec_ctx_t *c)
{
  swd_exectest_t *r = c->r;
  swd_err_t err;
  r->stage = "core state";
  err = readMem(c, DHCSR, &c->dhcsr_before); /* Clear historical reset flag. */
  if (err != SWD_OK) return err;
  err = state(c, false);
  if (err != SWD_OK) return err;
  c->dhcsr_before = r->dhcsr;
  if (r->dhcsr & C_STEP) return SWD_ERR_PRECONDITION;
  r->was_halted = (r->dhcsr & S_HALT) != 0;
  r->stage = "halt";
  r->core_restored = false;
  c->halted_by_us = true; /* Even failed completion might have halted it. */
  err = writeMem(c, DHCSR, DEBUG_KEY | C_DEBUGEN | C_HALT | (r->dhcsr & C_MASKINTS));
  if (err != SWD_OK) return err;
  err = waitState(c, S_HALT, 500);
  if (err != SWD_OK) return err;

  r->stage = "register backup";
  for (uint32_t i = 0; i < 5; i++)
  {
    err = regRead(c, saved_regs[i], &c->registers[i]);
    if (err != SWD_OK) return err;
  }
  err = regRead(c, 13, &c->sp_before);
  if (err != SWD_OK) return err;
  r->stage = "thread/Thumb state";
  if ((c->registers[4] & (XPSR_IPSR | XPSR_IT_ICI)) != 0
      || !(c->registers[4] & XPSR_THUMB)) return SWD_ERR_PRECONDITION;

  r->stage = "ITCM enabled";
  err = readMem(c, ITCMCR, &r->itcmcr);
  if (err != SWD_OK) return err;
  /* Never write code to address zero's flash alias when ITCM is disabled. */
  if (!(r->itcmcr & 1U) || !(r->itcmcr & 0x78U)) return SWD_ERR_PRECONDITION;
  r->stage = "MPU backup";
  err = readMem(c, MPU_CTRL, &r->mpu_ctrl);
  if (err != SWD_OK) return err;
  r->stage = "ITCM backup";
  for (uint32_t i = 0; i < 2; i++)
  {
    err = readMem(c, CODE_ADDR + 4U * i, &c->code_before[i]);
    if (err != SWD_OK) return err;
  }
  r->stage = "interrupt mask";
  err = writeMem(c, DHCSR, DEBUG_KEY | C_DEBUGEN | C_HALT | C_MASKINTS);
  if (err != SWD_OK) return err;
  r->stage = "MPU temporary disable";
  if (r->mpu_ctrl != 0)
  {
    r->mpu_restored = false;
    err = writeChecked(c, MPU_CTRL, 0);
    if (err != SWD_OK) return err;
  }
  r->stage = "ITCM code load";
  r->code_restored = false;
  for (uint32_t i = 0; i < 2; i++)
  {
    err = writeChecked(c, CODE_ADDR + 4U * i, code_words[i]);
    if (err != SWD_OK) return err;
  }
  r->stage = "register setup";
  const uint32_t values[5] = {7, 35, (CODE_ADDR + 4U) | 1U, CODE_ADDR, XPSR_THUMB};
  r->regs_restored = false;
  for (uint32_t i = 0; i < 5; i++)
  {
    err = regWrite(c, saved_regs[i], values[i]);
    if (err != SWD_OK) return err;
    uint32_t actual;
    err = regRead(c, saved_regs[i], &actual);
    if (err != SWD_OK) return err;
    if (actual != values[i]) return SWD_ERR_VERIFY;
  }
  return SWD_OK;
}

static swd_err_t execute(exec_ctx_t *c)
{
  swd_exectest_t *r = c->r;
  swd_err_t err;
  r->stage = "clear stop reason";
  err = writeMem(c, DFSR, 0x1F);
  if (err != SWD_OK) return err;
  r->stage = "execute / BKPT";
  r->started = true; /* Resume may take effect even if the SWD completion fails. */
  err = writeMem(c, DHCSR, DEBUG_KEY | C_DEBUGEN | C_MASKINTS);
  if (err != SWD_OK) return err;
  err = waitState(c, S_HALT, 500);
  if (err != SWD_OK) return err;
  err = readMem(c, DFSR, &r->dfsr);
  if (err != SWD_OK) return err;
  err = regRead(c, 15, &r->stop_pc);
  if (err != SWD_OK) return err;
  err = regRead(c, 0, &r->result);
  if (err != SWD_OK) return err;
  r->stage = "return value / stop location";
  /* Accept debugger PC conventions at BKPT or immediately after it. Do not
   * accept an arbitrary halt with a coincidentally correct R0. */
  if ((r->dfsr & 0x1FU) != 2U
      || (r->stop_pc != CODE_ADDR + 4U && r->stop_pc != CODE_ADDR + 6U))
    return SWD_ERR_PROTOCOL;
  return r->result == 42U ? SWD_OK : SWD_ERR_VERIFY;
}

static swd_err_t cleanup(exec_ctx_t *c)
{
  swd_exectest_t *r = c->r;
  if (!c->halted_by_us) return SWD_OK;
  r->cleanup_stage = "halt for restore";
  if (c->reset_seen) return SWD_ERR_CORE_STATE;
  swd_err_t err = swdDapClearErrors(&r->link);
  if (err != SWD_OK) return err;
  err = state(c, false);
  if (err != SWD_OK) return err;
  if (!(r->dhcsr & S_HALT))
  {
    err = writeMem(c, DHCSR, DEBUG_KEY | C_DEBUGEN | C_HALT | (r->dhcsr & C_MASKINTS));
    if (err != SWD_OK) return err;
    err = waitState(c, S_HALT, 500);
    if (err != SWD_OK) return err;
  }
  if (r->started)
  {
    uint32_t xpsr, sp, pc;
    r->cleanup_stage = "execution context check";
    err = regRead(c, 16, &xpsr); if (err != SWD_OK) return err;
    err = regRead(c, 13, &sp); if (err != SWD_OK) return err;
    err = regRead(c, 15, &pc); if (err != SWD_OK) return err;
    if ((xpsr & XPSR_IPSR) || sp != c->sp_before
        || pc < CODE_ADDR || pc > CODE_ADDR + 6U) return SWD_ERR_CORE_STATE;
  }
  r->cleanup_stage = "ITCM restore";
  if (!r->code_restored)
  {
    for (uint32_t i = 0; i < 2; i++)
    {
      err = writeChecked(c, CODE_ADDR + 4U * i, c->code_before[i]);
      if (err != SWD_OK) return err;
    }
    r->code_restored = true;
  }
  r->cleanup_stage = "register restore";
  if (!r->regs_restored)
  {
    for (uint32_t i = 0; i < 5; i++)
    {
      err = regWrite(c, saved_regs[i], c->registers[i]);
      if (err != SWD_OK) return err;
      uint32_t actual;
      err = regRead(c, saved_regs[i], &actual);
      if (err != SWD_OK) return err;
      if (actual != c->registers[i]) return SWD_ERR_VERIFY;
    }
    r->regs_restored = true;
  }
  r->cleanup_stage = "MPU restore";
  if (!r->mpu_restored)
  {
    err = writeChecked(c, MPU_CTRL, r->mpu_ctrl);
    if (err != SWD_OK) return err;
    r->mpu_restored = true;
  }
  r->cleanup_stage = "core state restore";
  /* Restore the interrupt mask while still halted, then restore run/halt.
   * These must be two distinct writes to DHCSR. */
  uint32_t mask = c->dhcsr_before & C_MASKINTS;
  err = writeMem(c, DHCSR, DEBUG_KEY | C_DEBUGEN | C_HALT | mask);
  if (err != SWD_OK) return err;
  err = state(c, true);
  if (err != SWD_OK) return err;
  if ((r->dhcsr & C_MASKINTS) != mask) return SWD_ERR_VERIFY;
  uint32_t control = (c->dhcsr_before & C_DEBUGEN) | mask;
  if (r->was_halted) control |= C_DEBUGEN | C_HALT;
  err = writeMem(c, DHCSR, DEBUG_KEY | control);
  if (err != SWD_OK) return err;
  uint32_t start = millis();
  do
  {
    err = state(c, false);
    if (err != SWD_OK) return err;
    if (((r->dhcsr & S_HALT) != 0) == r->was_halted)
    {
      r->core_restored = true;
      r->cleanup_stage = "complete";
      return SWD_OK;
    }
  } while ((uint32_t)(millis() - start) < 500U);
  return SWD_ERR_TIMEOUT;
}

swd_err_t swdExecTestH723(uint32_t khz, swd_exectest_t *r)
{
  if (r == NULL) { swdRelease(); return SWD_ERR_ARGUMENT; }
  memset(r, 0, sizeof(*r));
  r->link.ap = 0xFF;
  r->stage = "arguments";
  r->cleanup_stage = "not needed";
  r->code_addr = CODE_ADDR;
  r->code_restored = r->regs_restored = r->mpu_restored = r->core_restored = true;
  r->test_status = SWD_ERR_ARGUMENT;
  if (khz >= 10 && khz <= 500)
  {
    exec_ctx_t c = {0};
    c.r = r;
    swdPortBegin(khz);
    r->stage = "connect";
    r->test_status = swdH723Connect(&r->link);
    if (r->test_status == SWD_OK) r->test_status = prepare(&c);
    if (r->test_status == SWD_OK) r->test_status = execute(&c);
    r->cleanup_status = cleanup(&c);
    if (r->test_status == SWD_OK && r->cleanup_status == SWD_OK) r->stage = "complete";
  }
  swdRelease();
  return r->cleanup_status == SWD_OK ? r->test_status : SWD_ERR_RESTORE;
}
#endif
