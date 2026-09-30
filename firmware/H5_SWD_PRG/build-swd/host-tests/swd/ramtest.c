
#include "swd_dap.h"
#include "swd_transport.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
enum { NORMAL, ALREADY_HALTED, MASKED, BAD_TARGET, BACKUP_FAULT,
       PARTIAL_WRITE, VERIFY_BAD, RESTORE_WRITE_FAULT, RESTORE_VERIFY_BAD,
       RESET_DURING_TEST, RUN_DURING_TEST, RESUME_FAULT, HALT_TIMEOUT,
       HALT_WRITE_FAULT, RESET_LOOP, STEPPING, DISCONNECTED_DURING_TEST };
#define DHCSR 0xE000EDF0U
#define HALTED 0x20000U
#define RESET 0x2000000U
static unsigned mode, ticks, released, started, dhcsr, initial;
static unsigned ram[16], original[16], ram_writes, core_writes, resumed, injected;
uint32_t millis(void) { return ticks++; }
void swdPortBegin(uint32_t khz) { assert(khz == 100); started++; }
void swdRelease(void) { released++; }
swd_err_t swdH723Connect(swd_probe_t *p) {
  p->ap = 0; p->stage = "complete";
  return mode == BAD_TARGET ? SWD_ERR_MISMATCH : SWD_OK;
}
swd_err_t swdDapClearErrors(swd_probe_t *p) { (void)p; return SWD_OK; }
swd_err_t swdMemRead32(swd_probe_t *p, uint32_t addr, uint32_t *value) {
  (void)p;
  if (mode == DISCONNECTED_DURING_TEST && ram_writes == 4) return SWD_ERR_NORESP;
  if (addr == DHCSR) {
    if ((mode == RESET_DURING_TEST || mode == RUN_DURING_TEST)
        && ram_writes == 4 && !injected++)
      dhcsr = mode == RESET_DURING_TEST ? RESET : 0;
    *value = dhcsr;
    if (mode == RESET_LOOP) *value |= RESET;
    dhcsr &= ~RESET; /* Read clears the sticky historical reset indication. */
    return SWD_OK;
  }
  assert(addr >= 0x20000000 && addr < 0x20000040 && !(addr & 3));
  assert(dhcsr & HALTED);
  unsigned i = (addr - 0x20000000) / 4;
  if (mode == BACKUP_FAULT && i == 3 && !ram_writes) return SWD_ERR_FAULT;
  *value = ram[i];
  if (mode == VERIFY_BAD && ram_writes == 16 && !injected++) *value ^= 1;
  if (mode == RESTORE_VERIFY_BAD && ram_writes == 80) *value ^= 1;
  return SWD_OK;
}
swd_err_t swdMemWrite32(swd_probe_t *p, uint32_t addr, uint32_t value) {
  (void)p;
  if (addr == DHCSR) {
    core_writes++;
    assert((value & 0xFFFF0000) == 0xA05F0000);
    assert((value & 8) == (initial & 8)); /* Interrupt mask must be preserved. */
    assert(!(value & 4)); /* No stepping. */
    if (value & 2) {
      if (mode != HALT_TIMEOUT) dhcsr = HALTED | (value & 0xF);
      if (mode == HALT_WRITE_FAULT && core_writes == 1) return SWD_ERR_FAULT;
    } else {
      assert(dhcsr & HALTED);
      if (mode == RESUME_FAULT) return SWD_ERR_FAULT;
      dhcsr = value & 0xF; resumed++;
    }
    return SWD_OK;
  }
  assert(addr >= 0x20000000 && addr < 0x20000040 && !(addr & 3));
  assert(dhcsr & HALTED);
  ram_writes++;
  unsigned i = (addr - 0x20000000) / 4;
  ram[i] = value;
  /* Failure after a write has taken effect must still trigger full restore. */
  if (mode == PARTIAL_WRITE && ram_writes == 7) return SWD_ERR_FAULT;
  if (mode == RESTORE_WRITE_FAULT && ram_writes == 65) return SWD_ERR_FAULT;
  return SWD_OK;
}
static void setup(unsigned m) {
  mode = m; ticks = released = started = ram_writes = core_writes = resumed = injected = 0;
  initial = m == ALREADY_HALTED ? HALTED | 3 : m == MASKED ? 9 : m == STEPPING ? 5 : 0;
  dhcsr = initial | RESET; /* First read must consume the old reset bit. */
  for (unsigned i = 0; i < 16; i++) ram[i] = original[i] = 0xA7351200 ^ (i * 0x11111111U);
}
static swd_ramtest_t check(unsigned m, swd_err_t expected) {
  swd_ramtest_t r; setup(m);
  assert(swdRamTestH723(100, &r) == expected);
  assert(released == 1 && started == 1 && ticks < 1500);
  return r;
}
int main(void) {
  swd_ramtest_t r;
  const unsigned normal_modes[] = {NORMAL, ALREADY_HALTED, MASKED};
  for (unsigned i = 0; i < 3; i++) {
    r = check(normal_modes[i], SWD_OK);
    assert(r.passes == 4 && r.ram_restored && r.core_restored);
    assert(r.backup_saved && r.ram_written && !strcmp(r.stage, "complete"));
    assert(ram_writes == 80 && !memcmp(ram, original, sizeof(ram)));
    assert(dhcsr == initial);
    assert(resumed == (normal_modes[i] == ALREADY_HALTED ? 0U : 1U));
  }
  r = check(BAD_TARGET, SWD_ERR_MISMATCH);
  assert(!ram_writes && !core_writes);
  r = check(BACKUP_FAULT, SWD_ERR_FAULT);
  assert(!ram_writes && !r.backup_saved && r.core_restored);
  assert(!memcmp(ram, original, sizeof(ram)));
  r = check(PARTIAL_WRITE, SWD_ERR_FAULT);
  assert(ram_writes == 23 && r.ram_restored && r.core_restored && resumed == 1);
  assert(!memcmp(ram, original, sizeof(ram)));
  r = check(VERIFY_BAD, SWD_ERR_VERIFY);
  assert(r.fail_addr == 0x20000000 && r.expected == 0 && r.actual == 1);
  assert(r.ram_restored && r.core_restored && !memcmp(ram, original, sizeof(ram)));
  r = check(RESTORE_WRITE_FAULT, SWD_ERR_RESTORE);
  assert(!r.ram_restored && !r.core_restored && !resumed && (dhcsr & HALTED));
  r = check(RESTORE_VERIFY_BAD, SWD_ERR_RESTORE);
  assert(!r.ram_restored && !r.core_restored && !resumed && (dhcsr & HALTED));
  r = check(RESET_DURING_TEST, SWD_ERR_RESTORE);
  assert(r.test_status == SWD_ERR_CORE_STATE && ram_writes == 4 && !resumed);
  r = check(RUN_DURING_TEST, SWD_ERR_RESTORE);
  assert(r.test_status == SWD_ERR_CORE_STATE && ram_writes == 4 && !resumed);
  r = check(DISCONNECTED_DURING_TEST, SWD_ERR_RESTORE);
  assert(r.test_status == SWD_ERR_NORESP && ram_writes == 4 && !resumed);
  r = check(RESUME_FAULT, SWD_ERR_RESTORE);
  assert(r.ram_restored && !r.core_restored && !resumed);
  r = check(HALT_TIMEOUT, SWD_ERR_RESTORE);
  assert(r.test_status == SWD_ERR_TIMEOUT && !ram_writes);
  r = check(HALT_WRITE_FAULT, SWD_ERR_FAULT);
  assert(!ram_writes && r.core_restored && resumed == 1);
  r = check(RESET_LOOP, SWD_ERR_CORE_STATE);
  assert(!ram_writes && !core_writes);
  r = check(STEPPING, SWD_ERR_CORE_STATE);
  assert(!ram_writes && !core_writes);
  setup(NORMAL);
  assert(swdRamTestH723(0, &r) == SWD_ERR_ARGUMENT);
  assert(released == 1 && !started && !ram_writes && !core_writes);
  setup(NORMAL);
  assert(swdRamTestH723(100, NULL) == SWD_ERR_ARGUMENT);
  assert(released == 1 && !started);
  puts("SWD RAM test: patterns, content/state restoration, reset/link/write/cleanup failures passed");
}
