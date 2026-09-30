
#include "swd_dap.h"
#include "swd_transport.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DHCSR 0xE000EDF0U
#define DCRSR 0xE000EDF4U
#define DCRDR 0xE000EDF8U
#define DFSR 0xE000ED30U
#define ITCMCR 0xE000EF90U
#define MPU 0xE000ED94U
#define HALTED 0x20000U
#define READY 0x10000U
#define RESET 0x2000000U
enum { NORMAL, WAS_HALTED, MASKED, NO_MPU, NO_ITCM, HANDLER, IT_STATE,
       WRONG_RESULT, WRONG_PC, TIMEOUT, RESET_ON_RUN, STACK_CHANGED, EXCEPTION,
       REG_TIMEOUT, CODE_WRITE_FAIL, REG_SETUP_FAIL, MPU_WRITE_FAIL,
       CODE_RESTORE_FAIL, REG_RESTORE_FAIL, MPU_RESTORE_FAIL, MASK_RESTORE_FAIL,
       CODE_READBACK_BAD, BAD_TARGET, RUN_WRITE_FAIL };
static unsigned mode, ticks, releases, begun, dhcsr, initial_ctrl, mpu, initial_mpu;
static unsigned regs[21], original_regs[21], words[2], original_words[2];
static unsigned dfsr, dcrdr, executed, app_resumed, injected, mem_writes;
uint32_t millis(void) { return ticks++; }
void swdPortBegin(uint32_t khz) { assert(khz == 100); begun++; }
void swdRelease(void) { releases++; }
swd_err_t swdH723Connect(swd_probe_t *p) {
  p->ap = 0; p->stage = "complete";
  return mode == BAD_TARGET ? SWD_ERR_MISMATCH : SWD_OK;
}
swd_err_t swdDapClearErrors(swd_probe_t *p) { (void)p; return SWD_OK; }
swd_err_t swdMemRead32(swd_probe_t *p, uint32_t addr, uint32_t *value) {
  (void)p;
  switch (addr) {
    case DHCSR:
      *value = dhcsr | (mode == REG_TIMEOUT ? 0 : READY);
      dhcsr &= ~RESET;
      break;
    case DCRDR: *value = dcrdr; break;
    case DFSR: *value = dfsr; break;
    case ITCMCR: *value = mode == NO_ITCM ? 0x72 : 0x73; break;
    case MPU: *value = mpu; break;
    default:
      assert(addr == 0x100 || addr == 0x104);
      *value = words[(addr - 0x100)/4];
      if (mode == CODE_READBACK_BAD && words[0] == 0x47701840 && !injected++) *value ^= 1;
      break;
  }
  return SWD_OK;
}
swd_err_t swdMemWrite32(swd_probe_t *p, uint32_t addr, uint32_t value) {
  (void)p; mem_writes++;
  if (addr == DHCSR) {
    assert((value & 0xFFFF0000) == 0xA05F0000);
    unsigned ctrl = value & 0xF;
    if ((ctrl ^ dhcsr) & 8) {
      assert((dhcsr & HALTED) && (ctrl & 2)); /* Mask changes only while halted. */
      if (mode == MASK_RESTORE_FAIL && executed && !(ctrl & 8)) return SWD_ERR_FAULT;
    }
    if (ctrl & 2) { dhcsr = HALTED | ctrl; return SWD_OK; }
    if (regs[15] == 0x100) {
      assert(dhcsr & HALTED);
      assert(ctrl == 9 && mpu == 0 && regs[16] == 0x01000000);
      assert(words[0] == 0x47701840 && words[1] == 0xE7FEBE00);
      assert(regs[0] == 7 && regs[1] == 35 && regs[14] == 0x105);
      executed++;
      if (mode == TIMEOUT) { dhcsr = ctrl; return SWD_OK; }
      if (mode == RESET_ON_RUN) { dhcsr = RESET; return SWD_OK; }
      regs[0] += regs[1]; /* ADD, BX LR, BKPT; no stack operation. */
      regs[15] = regs[14] & ~1U;
      dfsr = 2; dhcsr = HALTED | ctrl;
      if (mode == WRONG_RESULT) regs[0]--;
      if (mode == WRONG_PC) regs[15] = 0x102;
      if (mode == STACK_CHANGED) regs[13] -= 8;
      if (mode == EXCEPTION) { regs[16] |= 3; regs[15] = 0x08001000; dfsr = 8; }
      if (mode == RUN_WRITE_FAIL) return SWD_ERR_FAULT;
    } else {
      assert(regs[15] == original_regs[15]);
      assert(!memcmp(words, original_words, sizeof(words)));
      assert(!memcmp(regs, original_regs, sizeof(regs)));
      assert(mpu == initial_mpu && ctrl == initial_ctrl);
      dhcsr = ctrl; app_resumed++;
    }
    return SWD_OK;
  }
  assert(dhcsr & HALTED);
  if (addr == DCRDR) dcrdr = value;
  else if (addr == DCRSR) {
    unsigned reg = value & 0xFFFF;
    assert(reg <= 20);
    if (value & 0x10000) {
      regs[reg] = dcrdr;
      if (mode == REG_SETUP_FAIL && !executed && reg == 0 && dcrdr == 7 && !injected++)
        return SWD_ERR_FAULT;
      if (mode == REG_RESTORE_FAIL && executed && reg == 0 && dcrdr == original_regs[0])
        return SWD_ERR_FAULT;
    } else dcrdr = regs[reg];
  } else if (addr == DFSR) dfsr &= ~value;
  else if (addr == MPU) {
    assert(value == 0 || value == initial_mpu);
    mpu = value;
    if (mode == MPU_WRITE_FAIL && !value && !injected++) return SWD_ERR_FAULT;
    if (mode == MPU_RESTORE_FAIL && executed && value) return SWD_ERR_FAULT;
  } else {
    assert(addr == 0x100 || addr == 0x104);
    words[(addr - 0x100)/4] = value;
    if (mode == CODE_WRITE_FAIL && value == 0x47701840 && !injected++) return SWD_ERR_FAULT;
    if (mode == CODE_RESTORE_FAIL && executed && value == original_words[0]) return SWD_ERR_FAULT;
  }
  return SWD_OK;
}
static void setup(unsigned m) {
  mode = m; ticks = releases = begun = executed = app_resumed = injected = mem_writes = 0;
  initial_ctrl = mode == WAS_HALTED ? 3 : mode == MASKED ? 9 : 0;
  dhcsr = initial_ctrl | RESET | (mode == WAS_HALTED ? HALTED : 0);
  mpu = initial_mpu = mode == NO_MPU ? 0 : 5;
  dfsr = 1; dcrdr = 0;
  for (unsigned i = 0; i < 21; i++) regs[i] = 0x12340000 + i * 4;
  regs[13] = 0x2001FFE0; regs[15] = 0x08001234;
  regs[16] = 0x21000000; /* Nonzero original APSR flags, Thumb, Thread mode. */
  if (mode == HANDLER) regs[16] |= 15;
  if (mode == IT_STATE) regs[16] |= 0x400;
  memcpy(original_regs, regs, sizeof(regs));
  words[0] = original_words[0] = 0xCAFEBABE;
  words[1] = original_words[1] = 0x13579BDF;
}
static swd_exectest_t check(unsigned m, swd_err_t expected) {
  swd_exectest_t r; setup(m);
  assert(swdExecTestH723(100, &r) == expected);
  assert(begun == 1 && releases == 1 && ticks < 1500);
  return r;
}
static void restored(swd_exectest_t *r) {
  assert(r->code_restored && r->regs_restored && r->mpu_restored && r->core_restored);
  assert(!memcmp(regs, original_regs, sizeof(regs)));
  assert(!memcmp(words, original_words, sizeof(words)));
  assert(mpu == initial_mpu && (dhcsr & 0xF) == initial_ctrl);
}
int main(void) {
  swd_exectest_t r;
  unsigned good[] = {NORMAL, WAS_HALTED, MASKED, NO_MPU};
  for (unsigned i = 0; i < 4; i++) {
    r = check(good[i], SWD_OK); restored(&r);
    assert(r.result == 42 && r.stop_pc == 0x104 && r.dfsr == 2 && executed == 1);
    assert(!strcmp(r.stage, "complete") && !strcmp(r.cleanup_stage, "complete"));
    assert(app_resumed == (good[i] == WAS_HALTED ? 0U : 1U));
  }
  unsigned preconditions[] = {NO_ITCM, HANDLER, IT_STATE};
  for (unsigned i = 0; i < 3; i++) {
    r = check(preconditions[i], SWD_ERR_PRECONDITION); restored(&r);
    assert(!executed && !r.started);
  }
  r = check(WRONG_RESULT, SWD_ERR_VERIFY); restored(&r);
  r = check(WRONG_PC, SWD_ERR_PROTOCOL); restored(&r);
  r = check(TIMEOUT, SWD_ERR_TIMEOUT); restored(&r);
  r = check(RESET_ON_RUN, SWD_ERR_RESTORE);
  assert(!app_resumed && !r.core_restored);
  r = check(STACK_CHANGED, SWD_ERR_RESTORE);
  assert(!app_resumed && (dhcsr & HALTED));
  r = check(EXCEPTION, SWD_ERR_RESTORE);
  assert(!app_resumed && (dhcsr & HALTED));
  r = check(REG_TIMEOUT, SWD_ERR_TIMEOUT); restored(&r); assert(!executed);
  unsigned failures[] = {CODE_WRITE_FAIL, REG_SETUP_FAIL, MPU_WRITE_FAIL, RUN_WRITE_FAIL};
  for (unsigned i = 0; i < 4; i++) { r = check(failures[i], SWD_ERR_FAULT); restored(&r); }
  unsigned cleanup_failures[] = {CODE_RESTORE_FAIL, REG_RESTORE_FAIL, MPU_RESTORE_FAIL, MASK_RESTORE_FAIL};
  for (unsigned i = 0; i < 4; i++) {
    r = check(cleanup_failures[i], SWD_ERR_RESTORE);
    assert(!app_resumed && !r.core_restored && (dhcsr & HALTED));
  }
  r = check(CODE_READBACK_BAD, SWD_ERR_VERIFY); restored(&r); assert(!executed);
  r = check(BAD_TARGET, SWD_ERR_MISMATCH); assert(!mem_writes && !executed);
  setup(NORMAL); assert(swdExecTestH723(0, &r) == SWD_ERR_ARGUMENT);
  assert(releases == 1 && !begun && !mem_writes);
  setup(NORMAL); assert(swdExecTestH723(100, NULL) == SWD_ERR_ARGUMENT);
  assert(releases == 1 && !begun && !mem_writes);
  puts("SWD execution: result/BKPT, code/register/MPU/core restoration and fault cases passed");
}
