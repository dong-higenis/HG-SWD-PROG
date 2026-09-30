
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
