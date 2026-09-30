#include "swd_dap.h"
#include "swd_transport.h"
#include <string.h>

#ifdef _USE_HW_SWD
/* H723 DTCM (ST stm32h723xx.h) avoids Cortex-M7 data-cache aliasing.
 * The live contents may belong to the application, so preserve all 64 bytes.
 * This is a bench diagnostic: halt does not freeze all peripherals or DMA. */
#define TEST_BASE      0x20000000UL
#define TEST_WORDS     16U
#define DHCSR          0xE000EDF0UL
#define DEBUG_KEY      0xA05F0000UL
#define C_DEBUGEN      (1UL << 0)
#define C_HALT         (1UL << 1)
#define C_STEP         (1UL << 2)
#define C_MASKINTS     (1UL << 3)
#define S_HALT         (1UL << 17)
#define S_LOCKUP       (1UL << 19)
#define S_RESET_ST     (1UL << 25)

static swd_err_t coreRead(swd_ramtest_t *r, bool require_halt)
{
  swd_err_t err = swdMemRead32(&r->link, DHCSR, &r->dhcsr_after);
  if (err != SWD_OK) return err;
  if (r->dhcsr_after & (S_RESET_ST | S_LOCKUP)) return SWD_ERR_CORE_STATE;
  if (require_halt && !(r->dhcsr_after & S_HALT)) return SWD_ERR_CORE_STATE;
  return SWD_OK;
}

static swd_err_t waitHalted(swd_ramtest_t *r)
{
  uint32_t start = millis();
  do
  {
    swd_err_t err = coreRead(r, false);
    if (err != SWD_OK) return err;
    if (r->dhcsr_after & S_HALT) return SWD_OK;
  } while ((uint32_t)(millis() - start) < 500U);
  return SWD_ERR_TIMEOUT;
}

static uint32_t pattern(uint32_t pass, uint32_t index)
{
  switch (pass)
  {
    case 0: return 0;
    case 1: return 0xFFFFFFFFUL;
    case 2: return 0x55AA55AAUL ^ (TEST_BASE + index * 4U);
    default: return ~(0x55AA55AAUL ^ (TEST_BASE + index * 4U));
  }
}

static swd_err_t restoreCore(swd_ramtest_t *r)
{
  swd_err_t err = coreRead(r, true);
  if (err != SWD_OK) return err;
  /* Preserve C_MASKINTS while halting and while resuming. Never change it
   * in the same write that releases C_HALT. C_STEP was rejected up front. */
  uint32_t controls = r->dhcsr_before & (C_DEBUGEN | C_MASKINTS);
  if (r->was_halted) controls |= C_DEBUGEN | C_HALT;
  err = swdMemWrite32(&r->link, DHCSR, DEBUG_KEY | controls);
  if (err != SWD_OK) return err;
  uint32_t start = millis();
  do
  {
    err = coreRead(r, false);
    if (err != SWD_OK) return err;
    if (((r->dhcsr_after & S_HALT) != 0) == r->was_halted)
    {
      r->core_restored = true;
      return SWD_OK;
    }
  } while ((uint32_t)(millis() - start) < 500U);
  return SWD_ERR_TIMEOUT;
}

static swd_err_t restoreRam(swd_ramtest_t *r, const uint32_t *backup)
{
  swd_err_t err = swdDapClearErrors(&r->link);
  if (err != SWD_OK) return err;
  for (uint32_t i = 0; i < TEST_WORDS; i++)
  {
    err = coreRead(r, true);
    if (err != SWD_OK) return err;
    err = swdMemWrite32(&r->link, TEST_BASE + i * 4U, backup[i]);
    if (err != SWD_OK) return err;
  }
  for (uint32_t i = 0; i < TEST_WORDS; i++)
  {
    uint32_t value;
    err = coreRead(r, true);
    if (err != SWD_OK) return err;
    err = swdMemRead32(&r->link, TEST_BASE + i * 4U, &value);
    if (err != SWD_OK) return err;
    if (value != backup[i]) return SWD_ERR_VERIFY;
  }
  r->ram_restored = true;
  return SWD_OK;
}

static swd_err_t testRam(swd_ramtest_t *r, uint32_t *backup)
{
  swd_err_t err;
  r->stage = "RAM backup";
  for (uint32_t i = 0; i < TEST_WORDS; i++)
  {
    err = coreRead(r, true);
    if (err != SWD_OK) return err;
    err = swdMemRead32(&r->link, TEST_BASE + i * 4U, &backup[i]);
    if (err != SWD_OK) return err;
  }
  r->backup_saved = true;
  for (uint32_t pass = 0; pass < 4; pass++)
  {
    r->stage = "RAM write";
    for (uint32_t i = 0; i < TEST_WORDS; i++)
    {
      err = coreRead(r, true);
      if (err != SWD_OK) return err;
      /* A failed AP completion can still have modified memory. */
      r->ram_written = true;
      err = swdMemWrite32(&r->link, TEST_BASE + i * 4U, pattern(pass, i));
      if (err != SWD_OK) return err;
    }
    r->stage = "RAM verify";
    for (uint32_t i = 0; i < TEST_WORDS; i++)
    {
      uint32_t value;
      err = coreRead(r, true);
      if (err != SWD_OK) return err;
      err = swdMemRead32(&r->link, TEST_BASE + i * 4U, &value);
      if (err != SWD_OK) return err;
      if (value != pattern(pass, i))
      {
        r->fail_addr = TEST_BASE + i * 4U;
        r->expected = pattern(pass, i);
        r->actual = value;
        return SWD_ERR_VERIFY;
      }
    }
    r->passes++;
  }
  return SWD_OK;
}

swd_err_t swdRamTestH723(uint32_t khz, swd_ramtest_t *r)
{
  if (r == NULL)
  {
    swdRelease();
    return SWD_ERR_ARGUMENT;
  }
  memset(r, 0, sizeof(*r));
  r->link.ap = 0xFF;
  r->stage = "arguments";
  r->address = TEST_BASE;
  r->size = TEST_WORDS * 4U;
  r->test_status = SWD_ERR_ARGUMENT;
  uint32_t backup[TEST_WORDS];
  bool halt_requested = false;
  if (khz < 10 || khz > 500) goto done;
  swdPortBegin(khz);
  r->stage = "connect";
  r->test_status = swdH723Connect(&r->link);
  if (r->test_status != SWD_OK) goto done;

  r->stage = "core state";
  /* Read once to clear a historical sticky reset indication, then require a
   * stable state. Later resets are treated as failures, never resumed. */
  r->test_status = swdMemRead32(&r->link, DHCSR, &r->dhcsr_before);
  if (r->test_status != SWD_OK) goto done;
  r->test_status = coreRead(r, false);
  if (r->test_status != SWD_OK) goto done;
  r->dhcsr_before = r->dhcsr_after;
  if (r->dhcsr_before & C_STEP)
  {
    r->test_status = SWD_ERR_CORE_STATE;
    goto done;
  }
  r->core_saved = true;
  r->was_halted = (r->dhcsr_before & S_HALT) != 0;
  r->stage = "halt";
  halt_requested = true;
  r->test_status = swdMemWrite32(&r->link, DHCSR, DEBUG_KEY | C_DEBUGEN | C_HALT
                               | (r->dhcsr_before & C_MASKINTS));
  if (r->test_status != SWD_OK) goto cleanup;
  r->test_status = waitHalted(r);
  if (r->test_status != SWD_OK) goto cleanup;
  r->test_status = testRam(r, backup);

cleanup:
  /* Do not overwrite a rebooted/running target with a stale RAM snapshot.
   * The failure stage stays intact while cleanup is reported separately. */
  if (r->test_status == SWD_ERR_CORE_STATE)
  {
    r->cleanup_status = SWD_ERR_CORE_STATE;
    goto done;
  }
  if (r->ram_written)
  {
    r->cleanup_status = restoreRam(r, backup);
    if (r->cleanup_status != SWD_OK) goto done;
  }
  else if (halt_requested)
  {
    r->cleanup_status = swdDapClearErrors(&r->link);
    if (r->cleanup_status != SWD_OK) goto done;
  }
  r->cleanup_status = restoreCore(r);
  if (r->test_status == SWD_OK && r->cleanup_status == SWD_OK) r->stage = "complete";
done:
  swdRelease();
  return r->cleanup_status == SWD_OK ? r->test_status : SWD_ERR_RESTORE;
}
#endif
