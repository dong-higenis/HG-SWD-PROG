"""Host checks for SWD wire ownership, posted AP reads, and probe failures."""
from pathlib import Path
import argparse
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default=shutil.which('gcc'))
args = parser.parse_args()
if not args.cc:
    parser.error('Provide --cc with a native GCC compiler')
work = root / 'build-swd' / 'host-tests' / 'swd'
work.mkdir(parents=True, exist_ok=True)
(work / 'hw_def.h').write_text('''#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define _USE_HW_SWD
uint32_t millis(void);
''', encoding='utf-8')


def run(name, source, test):
    path = work / (name + '.c')
    path.write_text(test, encoding='utf-8')
    exe = work / (name + '.exe')
    subprocess.run([
        args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(work), '-I' + str(root / 'src/common/hw/include'),
        '-I' + str(root / 'src/hw/driver/swd'),
        str(root / 'src/hw/driver/swd' / source), str(path), '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)


run('wire', 'swd_transport.c', r'''
#include "swd_transport.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool output, clk, data_pin;
static unsigned rises, nr, at;
static unsigned bits[64];
void swdPortActivity(void) {}
static bool edge_output[256], edge_data[256];
void swdClockWrite(bool high) {
  if (high && !clk) {
    assert(rises < 256);
    edge_output[rises] = output; edge_data[rises++] = data_pin;
  }
  clk = high;
}
void swdDataOutput(bool enable) { output = enable; }
void swdDataWrite(bool high) { data_pin = high; }
bool swdDataRead(void) {
  assert(!output && !clk); /* Buffered input is only sampled while released. */
  assert(at < nr); return bits[at++];
}
void swdPortDelay(void) {}
static void reset(void) {
  output = true; clk = true; data_pin = true; rises = nr = at = 0;
  memset(edge_output, 0, sizeof(edge_output));
}
static void queue(unsigned value, unsigned count) {
  for (unsigned i = 0; i < count; i++) bits[nr++] = (value >> i) & 1;
}
static unsigned decode(unsigned start, unsigned count) {
  unsigned value = 0;
  for (unsigned i = 0; i < count; i++) value |= (unsigned)edge_data[start+i] << i;
  return value;
}
int main(void) {
  reset(); swdWireConnect();
  assert(rises == 136);
  for (unsigned i = 0; i < 56; i++) assert(edge_data[i] && edge_output[i]);
  assert(decode(56, 16) == 0xE79E);
  for (unsigned i = 72; i < 128; i++) assert(edge_data[i]);
  assert(decode(128, 8) == 0);

  reset(); queue(1, 3); queue(0x2BA01477, 32); queue(0, 1);
  unsigned value = 0;
  assert(swdWireTransfer(false, true, 0, &value) == SWD_ACK_OK);
  assert(value == 0x2BA01477 && at == nr && rises == 54);
  assert(decode(0, 8) == 0xA5); /* Known DPIDR request packet. */
  for (unsigned i = 0; i < 8; i++) assert(edge_output[i]);
  for (unsigned i = 8; i < 46; i++) assert(!edge_output[i]);
  for (unsigned i = 46; i < 54; i++) assert(edge_output[i] && !edge_data[i]);

  reset(); queue(1, 3); queue(0x2BA01477, 32); queue(1, 1);
  value = 0xDEADBEEF;
  assert(swdWireTransfer(false, true, 0, &value) == SWD_ACK_PARITY);
  assert(value == 0xDEADBEEF); /* Never publish corrupt data. */

  reset(); queue(1, 3); value = 0x80000001;
  assert(swdWireTransfer(true, false, 0xC, &value) == SWD_ACK_OK);
  assert(decode(0, 8) == 0xBB); /* AP DRW write request. */
  for (unsigned i = 8; i < 13; i++) assert(!edge_output[i]);
  for (unsigned i = 13; i < 46; i++) assert(edge_output[i]);
  assert(decode(13, 32) == value && !edge_data[45]);

  const unsigned acks[] = {2, 4, 7, 0};
  for (unsigned j = 0; j < 4; j++) {
    reset(); queue(acks[j], 3); value = 0x1234;
    assert(swdWireTransfer(false, true, 0, &value) == acks[j]);
    assert(value == 0x1234 && at == 3 && output);
    assert(rises == (j < 2 ? 21U : 54U));
  }
  puts("SWD wire: request bits, turnaround, parity, WAIT/FAULT/no-response passed");
}
''')

run('probe', 'swd_probe.c', r'''
#include "swd_transport.h"
#include "swd_dap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
enum { NORMAL, DISCONNECTED, BAD_PARITY, WAIT_ONCE, WAIT_FOREVER,
       POWER_TIMEOUT, NO_AP, WRONG_CORE, WRONG_DEVICE, AP_FAULT,
       STICKY_FAULT, SECOND_AP, STUCK_AP };
static unsigned mode, ticks, transfers, releases, connects, begun;
static unsigned select_reg, posted, tar, csw, wait_sent;
static unsigned allow_write, pending_write, completed_write, write_value, fail_completion;
uint32_t millis(void) { return ticks++; }
void swdPortBegin(uint32_t khz) { assert(khz >= 10 && khz <= 500); begun++; }
void swdRelease(void) { releases++; }
void swdWireConnect(void) { connects++; }
uint32_t swdWireTransfer(bool ap, bool read, uint8_t addr, uint32_t *data) {
  transfers++;
  if (mode == DISCONNECTED) return SWD_ACK_NORESP;
  if (mode == BAD_PARITY) return SWD_ACK_PARITY;
  if (mode == WAIT_FOREVER) return SWD_ACK_WAIT;
  if (mode == WAIT_ONCE && !wait_sent++) return SWD_ACK_WAIT;
  if (!ap) {
    if (read) {
      if (addr == 0) *data = 0x6BA02477;
      else if (addr == 4) *data = mode == POWER_TIMEOUT ? 0x50000000 : 0xF0000000;
      else {
        assert(addr == 12);
        if (pending_write) {
          if (fail_completion) return SWD_ACK_FAULT;
          pending_write = 0; completed_write++;
        }
        *data = posted;
      }
      if (addr == 4) assert(!pending_write);
      if (addr == 4 && tar && mode == STICKY_FAULT) *data |= 0x20;
    } else {
      if (addr == 8) select_reg = *data;
      else if (addr == 0) assert(*data == 0x1E);
      else { assert(addr == 4 && *data == 0x50000000); }
    }
    return SWD_ACK_OK;
  }
  unsigned reg = (select_reg & 0xF0) | addr;
  unsigned ap_index = select_reg >> 24;
  if (mode == AP_FAULT) return SWD_ACK_FAULT;
  if (read) {
    unsigned result;
    if (reg == 0xFC) {
      result = mode == NO_AP || ap_index != (mode == SECOND_AP ? 1U : 0U)
             ? 0 : 0x84770001;
    } else {
      assert(reg == 12 && csw == 0x23000002);
      if (tar == 0xE000ED00)
        result = mode == WRONG_CORE ? 0x410FC241 : 0x411FC272;
      else {
        assert(tar == 0x5C001000);
        result = mode == STUCK_AP ? 0x411FC271 :
                 mode == WRONG_DEVICE ? 0x10000450 : 0x10016483;
      }
    }
    *data = 0xDEADBEEF; /* AP data is stale: only RDBUFF gives the current value. */
    posted = result;
  } else {
    /* No write to DRW, halt/reset register, target RAM or flash is allowed. */
    if (reg == 0) csw = *data;
    else if (reg == 4) tar = *data;
    else {
      assert(allow_write && reg == 12 && tar == 0x20000000);
      pending_write = 1; write_value = *data;
    }
  }
  return SWD_ACK_OK;
}
static void reset(unsigned m) {
  mode = m; ticks = transfers = releases = connects = begun = 0;
  select_reg = posted = tar = csw = wait_sent = 0;
  allow_write = pending_write = completed_write = write_value = fail_completion = 0;
}
static swd_probe_t check(unsigned m, swd_err_t expected, const char *stage) {
  swd_probe_t p; reset(m);
  assert(swdProbeH723(100, &p) == expected);
  assert(releases == 1 && begun == 1 && transfers < 1000);
  assert(!strcmp(p.stage, stage));
  return p;
}
int main(void) {
  swd_probe_t p = check(NORMAL, SWD_OK, "complete");
  assert(p.dpidr == 0x6BA02477 && p.cpuid == 0x411FC272);
  assert(p.ap == 0 && p.ap_idr == 0x84770001 && p.device_id == 0x10016483);
  p = check(SECOND_AP, SWD_OK, "complete"); assert(p.ap == 1);
  p = check(WAIT_ONCE, SWD_OK, "complete"); assert(p.wait_count == 1);
  check(DISCONNECTED, SWD_ERR_NORESP, "DPIDR"); assert(connects == 3);
  check(BAD_PARITY, SWD_ERR_PARITY, "DPIDR");
  check(WAIT_FOREVER, SWD_ERR_WAIT, "DPIDR"); assert(transfers == 192);
  check(POWER_TIMEOUT, SWD_ERR_TIMEOUT, "DP power-up");
  check(NO_AP, SWD_ERR_NO_AP, "AP scan");
  check(WRONG_CORE, SWD_ERR_MISMATCH, "CPUID");
  check(WRONG_DEVICE, SWD_ERR_MISMATCH, "DBGMCU_IDCODE");
  check(STUCK_AP, SWD_ERR_MISMATCH, "DBGMCU_IDCODE");
  check(AP_FAULT, SWD_ERR_FAULT, "AP IDR");
  check(STICKY_FAULT, SWD_ERR_FAULT, "CPUID");
  reset(NORMAL); assert(swdProbeH723(0, &p) == SWD_ERR_ARGUMENT);
  assert(releases == 1 && !begun && !transfers);
  reset(NORMAL); assert(swdProbeH723(100, NULL) == SWD_ERR_ARGUMENT);
  assert(releases == 1 && !begun && !transfers);
  reset(NORMAL); allow_write = 1; p.ap = 0;
  assert(swdMemWrite32(&p, 0x20000000, 0x13579BDF) == SWD_OK);
  assert(completed_write == 1 && write_value == 0x13579BDF);
  reset(NORMAL); allow_write = fail_completion = 1;
  assert(swdMemWrite32(&p, 0x20000000, 123) == SWD_ERR_FAULT);
  assert(!completed_write);
  reset(NORMAL);
  assert(swdMemWrite32(&p, 0x20000001, 0) == SWD_ERR_ARGUMENT);
  assert(swdMemRead32(&p, 0x20000001, &tar) == SWD_ERR_ARGUMENT);
  assert(swdMemRead32(&p, 0x20000000, NULL) == SWD_ERR_ARGUMENT);
  assert(!transfers);
  puts("SWD probe: posted reads, AP selection, IDs, bounded failures and release passed");
}
''')

run('ramtest', 'swd_ramtest.c', r'''
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
''')

run('exectest', 'swd_exectest.c', r'''
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
''')

run('burst', 'swd_probe.c', r'''
#include "swd_dap.h"
#include "swd_transport.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t memory[2048], tar, csw, posted, transfers, tar_writes, status_reads, flushes;
static unsigned mode, waits;
uint32_t millis(void) { return transfers; }
void swdPortBegin(uint32_t khz) { (void)khz; }
void swdRelease(void) {}
void swdWireConnect(void) {}
uint32_t swdWireTransfer(bool ap,bool read,uint8_t addr,uint32_t *v) {
  transfers++;
  if(mode==1 && transfers==7) { waits++; return SWD_ACK_WAIT; }
  if(mode==2)return SWD_ACK_WAIT;
  if(mode==3 && transfers==15)return SWD_ACK_FAULT;
  if(!ap) {
    if(read && addr==12) { *v=posted; flushes++; }
    else if(read && addr==4) { *v=mode==4?0x20:0;status_reads++; }
    else { assert(!read && addr==8 && *v==0); }
    return SWD_ACK_OK;
  }
  if(!read && addr==0)csw=*v;
  else if(!read && addr==4) { tar=*v;tar_writes++; }
  else {
    assert(addr==12 && csw==0x23000012);
    assert(tar>=0x20000000 && tar<0x20002000);
    unsigned index=(tar-0x20000000)/4;
    if(read) { uint32_t old=posted;posted=memory[index];*v=old; }
    else memory[index]=*v;
    tar=(tar&~1023U)|((tar+4)&1023); // Hardware wraps at 1KiB.
  }
  return SWD_ACK_OK;
}
static void setup(unsigned m) {
  mode=m;tar=csw=posted=transfers=tar_writes=status_reads=flushes=waits=0;
  for(unsigned i=0;i<2048;i++)memory[i]=0x3948a721U^(i*7919);
}
int main(void) {
  swd_probe_t p={0};uint32_t buf[1000], expected[1000];
  for(unsigned m=0;m<2;m++) {
    setup(m);memcpy(expected,memory+252,sizeof(expected));
    assert(swdMemReadWords(&p,0x200003f0,buf,1000)==SWD_OK);
    assert(!memcmp(buf,expected,sizeof(buf)) && tar_writes==5 && status_reads==5 && flushes==5);
    assert(waits==m);
    setup(m);for(unsigned i=0;i<1000;i++)buf[i]=~i;
    assert(swdMemWriteWords(&p,0x200003f0,buf,1000)==SWD_OK);
    assert(!memcmp(memory+252,buf,sizeof(buf)) && tar_writes==5 && status_reads==5 && flushes==5);
  }
  setup(2);assert(swdMemReadWords(&p,0x20000000,buf,10)==SWD_ERR_WAIT && transfers==64);
  setup(3);assert(swdMemWriteWords(&p,0x20000000,buf,100)==SWD_ERR_FAULT);
  setup(4);assert(swdMemReadWords(&p,0x20000000,buf,100)==SWD_ERR_FAULT);
  setup(0);assert(swdMemReadWords(&p,0x20000001,buf,1)==SWD_ERR_ARGUMENT);
  assert(swdMemWriteWords(&p,0xfffffff0,buf,100)==SWD_ERR_ARGUMENT && !transfers);
  puts("SWD burst: posted pipeline, 1KiB wrap boundaries, WAIT retry, sticky errors and bounds passed");
}
''')
