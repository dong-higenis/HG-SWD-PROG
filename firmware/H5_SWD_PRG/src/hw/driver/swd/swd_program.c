/* H723 1MiB internal flash, pinned ST 0x483 loader. Reset-based programming
 * deliberately discards the old application context. No option-byte writes.
 * Registers: ST RM0468 / stm32h723xx.h; core debug: Arm DDI0489. */
#include "swd_program.h"
#include "swd_dap.h"
#include "swd_transport.h"
#include <string.h>

#define DHCSR        0xE000EDF0U
#define DCRSR        0xE000EDF4U
#define DCRDR        0xE000EDF8U
#define DEMCR        0xE000EDFCU
#define DFSR         0xE000ED30U
#define AIRCR        0xE000ED0CU
#define KEY          0xA05F0000U
#define HALTED       (1U << 17)
#define REGRDY       (1U << 16)
#define RESET        (1U << 25)
#define LOCKUP       (1U << 19)
#define TARGET_FLASH 0x08000000U
#define FLASH_CR     0x5200200CU
#define FLASH_SR     0x52002010U
#define FLASH_CCR    0x52002014U
#define FLASH_ERRORS 0x17EE0000U
#define BUFFER       0x24002000U
#define STACK_BOTTOM 0x24004000U
#define STACK_TOP    0x24008000U
#define CANARY       0x51A7C0DEU
#define CHUNK        4096U

static uint32_t data[CHUNK / 4];

static uint32_t actual[CHUNK / 4];

typedef struct
{
  const swd_program_input_t *in;
  swd_program_result_t      *r;
  uint32_t                   done;
  uint32_t                   total;
  uint32_t                   dbg3;
  uint32_t                   dbg4;
  uint32_t                   phase_start;
  bool                       debug_saved;
  bool                       touched;
  bool                       recovering;
  bool                       erase_only;
} context_t;

#define TRY(x)           \
  do                     \
  {                      \
    swd_err_t err = (x); \
    if (err != SWD_OK)   \
    {                    \
      return err;        \
    }                    \
  } while (0)

static swd_err_t swdProgramIoResult(context_t *c, swd_err_t e, const char *op, uint32_t a)
{
  if (e != SWD_OK && !c->recovering)
  {
    c->r->failed_io         = op;
    c->r->failed_io_address = a;
    c->r->failed_ack        = c->r->link.last_ack;
    c->r->failed_ctrl       = c->r->link.ctrl_stat;
  }
  return e;
}

static swd_err_t swdProgramRead32(context_t *c, uint32_t a, uint32_t *v)
{
  return swdProgramIoResult(c, swdMemRead32(&c->r->link, a, v), "READ32", a);
}

static swd_err_t swdProgramWrite32(context_t *c, uint32_t a, uint32_t v)
{
  return swdProgramIoResult(c, swdMemWrite32(&c->r->link, a, v), "WRITE32", a);
}

static swd_err_t swdProgramReadWords(context_t *c, uint32_t a, uint32_t *v, uint32_t n)
{
  return swdProgramIoResult(c, swdMemReadWords(&c->r->link, a, v, n), "READ BLOCK", a);
}

static swd_err_t swdProgramWriteWords(context_t *c, uint32_t a, const uint32_t *v, uint32_t n)
{
  return swdProgramIoResult(c, swdMemWriteWords(&c->r->link, a, v, n), "WRITE BLOCK", a);
}

static uint32_t swdProgramCrcUpdate(uint32_t crc, const uint8_t *p, uint32_t n)
{
  while (n--)
  {
    crc ^= *p++;
    for (unsigned i = 0; i < 8; i++)
    {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
    }
  }
  return crc;
}

static void swdProgramNotify(context_t *c)
{
  if (c->in->progress && !c->in->progress(c->in->context, c->r->stage, c->done, c->total))
  {
    c->r->cancelled = true;
  }
}

static void swdProgramFinishPhase(context_t *c)
{
  uint32_t elapsed = millis() - c->phase_start;
  if (!strcmp(c->r->stage, "ERASE"))
  {
    c->r->erase_ms = elapsed;
  }
  if (!strcmp(c->r->stage, "WRITE"))
  {
    c->r->write_ms = elapsed;
  }
  if (!strcmp(c->r->stage, "VERIFY") || !strcmp(c->r->stage, "BLANK CHECK"))
  {
    c->r->verify_ms = elapsed;
  }
}

static swd_err_t swdProgramSetStage(context_t *c, const char *name, uint32_t done, uint32_t total)
{
  c->r->failed_io          = NULL;
  c->r->flash_sr_valid     = false;
  c->r->flash_status_error = false;
  if (strcmp(c->r->stage, name))
  {
    swdProgramFinishPhase(c);
    c->phase_start = millis();
  }
  c->r->stage = name;
  c->done     = done;
  c->total    = total;
  swdProgramNotify(c);
  return c->r->cancelled ? SWD_ERR_PRECONDITION : SWD_OK;
}

static swd_err_t swdProgramCheckState(context_t *c, bool halt, uint32_t *value)
{
  uint32_t v;
  TRY(swdProgramRead32(c, DHCSR, &v));
  if (value)
  {
    *value = v;
  }
  return ((v & (RESET | LOCKUP)) || (halt && !(v & HALTED))) ? SWD_ERR_CORE_STATE : SWD_OK;
}

static swd_err_t swdProgramWaitRegister(context_t *c)
{
  uint32_t start = millis(), v;
  do
  {
    TRY(swdProgramCheckState(c, true, &v));
    if (v & REGRDY)
    {
      return SWD_OK;
    }
  } while ((uint32_t)(millis() - start) < 100);
  return SWD_ERR_TIMEOUT;
}

static swd_err_t swdProgramReadRegister(context_t *c, uint32_t n, uint32_t *v)
{
  TRY(swdProgramCheckState(c, true, NULL));
  TRY(swdProgramWrite32(c, DCRSR, n));
  TRY(swdProgramWaitRegister(c));
  return swdProgramRead32(c, DCRDR, v);
}

static swd_err_t swdProgramWriteRegister(context_t *c, uint32_t n, uint32_t v)
{
  uint32_t check;
  TRY(swdProgramCheckState(c, true, NULL));
  TRY(swdProgramWrite32(c, DCRDR, v));
  TRY(swdProgramWrite32(c, DCRSR, 0x10000 | n));
  TRY(swdProgramWaitRegister(c));
  TRY(swdProgramReadRegister(c, n, &check));
  return check == v ? SWD_OK : SWD_ERR_VERIFY;
}

static swd_err_t swdProgramResetHalt(context_t *c)
{
  uint32_t v;
  uint32_t dfsr;
  uint32_t start;
  c->touched = true;
  TRY(swdProgramRead32(c, DHCSR, &v));
  TRY(swdProgramWrite32(c, DHCSR, KEY | 3 | (v & 8U)));
  start = millis();
  do
  {
    TRY(swdProgramRead32(c, DHCSR, &v));
    if (v & HALTED)
    {
      break;
    }
  } while ((uint32_t)(millis() - start) < 500);
  if (!(v & HALTED))
  {
    return SWD_ERR_TIMEOUT;
  }
  TRY(swdProgramWrite32(c, DHCSR, KEY | 3));
  TRY(swdProgramWrite32(c, DEMCR, 0x7F1));       /* Reset, fault vector catches. */
  TRY(swdProgramWrite32(c, DFSR, 31));
  TRY(swdProgramRead32(c, DHCSR, &v));           /* Consume old reset indication. */
  c->r->reset_attempted = true;
  (void)swdProgramWrite32(c, AIRCR, 0x05FA0004); /* Reset can drop this write's completion. */
  bool reset_seen = false;
  start           = millis();
  do
  {
    if (swdProgramRead32(c, DHCSR, &v) != SWD_OK)
    {
      (void)swdH723Connect(&c->r->link);
      continue;
    }
    if (v & RESET)
    {
      reset_seen = true;
    }
    if (reset_seen && (v & HALTED))
    {
      TRY(swdProgramRead32(c, DFSR, &dfsr));
      if (!(dfsr & 8U))
      {
        return SWD_ERR_CORE_STATE;
      }
      TRY(swdProgramRead32(c, DHCSR, &v));
      if ((v & RESET) || !(v & HALTED))
      {
        continue;
      }
      TRY(swdProgramWrite32(c, DEMCR, 0x7F0));
      TRY(swdProgramWrite32(c, DFSR, 31));
      c->r->failed_io = NULL; /* A dropped reset completion was recovered. */
      return SWD_OK;
    }
    swdProgramNotify(c);
  } while ((uint32_t)(millis() - start) < 1500);
  return SWD_ERR_TIMEOUT;
}

static swd_err_t swdProgramFlashStatus(context_t *c)
{
  c->r->flash_sr_valid = false;
  TRY(swdProgramRead32(c, FLASH_SR, &c->r->flash_sr));
  c->r->flash_sr_valid     = true;
  c->r->flash_status_error = (c->r->flash_sr & (FLASH_ERRORS | 7U)) != 0;
  return c->r->flash_status_error ? SWD_ERR_FAULT : SWD_OK;
}

/* The old application has already been reset/halted. BUFFER is scratch RAM.
 * Validate long bursts in both directions at each candidate rate before any
 * flash unlock/erase, rather than relying on a short successful ID read. */
static swd_err_t swdProgramTuneSpeed(context_t *c)
{
  static const uint32_t rates[] = {12000, 8000, 4000, 2000, 1000, 500, 100};
  uint32_t              start   = millis();
  for (unsigned rate = 0; rate < sizeof(rates) / sizeof(rates[0]); rate++)
  {
    if (rates[rate] > swdPortMaxKhz())
    {
      continue;
    }
    TRY(swdProgramSetStage(c, "LINK SPEED TEST", rate, 7));
    swdPortSetSpeed(rates[rate]);
    swd_err_t e = swdH723Connect(&c->r->link);
    if (e == SWD_OK)
    {
      e = swdProgramCheckState(c, true, NULL);
    }
    for (unsigned pass = 0; e == SWD_OK && pass < 3; pass++)
    {
      uint32_t seed = 0x937A21B5U ^ (pass * 0xAAAAAAAAU);
      for (unsigned i = 0; i < CHUNK / 4; i++)
      {
        seed    ^= seed << 13;
        seed    ^= seed >> 17;
        seed    ^= seed << 5;
        data[i]  = seed;
      }
      e = swdProgramWriteWords(c, BUFFER, data, CHUNK / 4);
      if (e == SWD_OK)
      {
        e = swdProgramReadWords(c, BUFFER, actual, CHUNK / 4);
      }
      if (e == SWD_OK && memcmp(data, actual, CHUNK))
      {
        e = SWD_ERR_VERIFY;
      }
      if (e == SWD_OK)
      {
        e = swdProgramCheckState(c, true, NULL);
      }
      swdProgramNotify(c);
      if (c->r->cancelled)
      {
        return SWD_ERR_PRECONDITION;
      }
    }
    if (e == SWD_OK)
    {
      c->r->clock_khz    = rates[rate];
      c->r->link_test_ms = millis() - start;
      return SWD_OK;
    }
    /* Resynchronize at a known conservative rate before retrying. Never retry
     * a flash command: all retries here occur before loading/executing code. */
    swdPortSetSpeed(100);
    TRY(swdH723Connect(&c->r->link));
    TRY(swdProgramCheckState(c, true, NULL));
    if (e == SWD_ERR_CORE_STATE)
    {
      return e;
    }
  }
  c->r->link_test_ms = millis() - start;
  return SWD_ERR_VERIFY;
}

static swd_err_t swdProgramCallLoader(context_t *c, uint32_t entry, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                                      uint32_t timeout)
{
  TRY(swdProgramCheckState(c, true, NULL));
  TRY(swdProgramWrite32(c, DHCSR, KEY | 11)); /* Change C_MASKINTS only while halted. */
  TRY(swdProgramWriteRegister(c, 20, 1));     /* Privileged MSP, PRIMASK=1, BASEPRI/FAULTMASK=0. */
  TRY(swdProgramWriteRegister(c, 17, STACK_TOP));
  TRY(swdProgramWriteRegister(c, 0, r0));
  TRY(swdProgramWriteRegister(c, 1, r1));
  TRY(swdProgramWriteRegister(c, 2, r2));
  TRY(swdProgramWriteRegister(c, 3, r3));
  TRY(swdProgramWriteRegister(c, 14, 0x24000001));
  TRY(swdProgramWriteRegister(c, 15, entry & ~1U));
  TRY(swdProgramWriteRegister(c, 16, 1U << 24));
  TRY(swdProgramWrite32(c, DFSR, 31));
  TRY(swdProgramWrite32(c, DHCSR, KEY | 9));
  uint32_t start = millis(), v;
  do
  {
    TRY(swdProgramCheckState(c, false, &v));
    swdProgramNotify(c); /* Latch stop request, let an in-flight flash operation finish. */
    if (v & HALTED)
    {
      TRY(swdProgramRead32(c, DFSR, &c->r->dfsr));
      TRY(swdProgramReadRegister(c, 15, &c->r->pc));
      TRY(swdProgramReadRegister(c, 0, &c->r->result));
      TRY(swdProgramReadRegister(c, 16, &v));
      if ((v & 0x1FF) || (c->r->dfsr & 31) != 2 || (c->r->pc != 0x24000000 && c->r->pc != 0x24000002))
      {
        return SWD_ERR_CORE_STATE;
      }
      TRY(swdProgramReadRegister(c, 17, &v));
      if (v != STACK_TOP)
      {
        return SWD_ERR_CORE_STATE;
      }
      TRY(swdProgramRead32(c, STACK_BOTTOM, &v));
      if (v != CANARY)
      {
        return SWD_ERR_VERIFY;
      }
      if (c->r->result != 1)
      {
        return SWD_ERR_VERIFY;
      }
      return swdProgramFlashStatus(c);
    }
  } while ((uint32_t)(millis() - start) < timeout);
  return SWD_ERR_TIMEOUT;
}

static swd_err_t swdProgramExecute(context_t *c)
{
  uint32_t v;
  TRY(swdProgramSetStage(c, "CONNECT", 0, 1));
  TRY(swdH723Connect(&c->r->link));
  TRY(swdProgramRead32(c, 0x1FF1E880, &v));
  if ((v & 0xFFFF) != 1024)
  {
    return SWD_ERR_MISMATCH;
  }
  /* Do not remove protection or touch option bytes. */
  TRY(swdProgramRead32(c, 0x5200201C, &v));
  if ((v & 0xFF00) != 0xAA00)
  {
    return SWD_ERR_PRECONDITION;
  }
  uint32_t sectors = c->erase_only ? 8 : (c->in->image_size + 0x1FFFF) / 0x20000;
  TRY(swdProgramRead32(c, 0x52002038, &v));
  if ((v & ((1U << sectors) - 1)) != ((1U << sectors) - 1))
  {
    return SWD_ERR_PRECONDITION;
  }
  TRY(swdProgramRead32(c, 0x5C001034, &c->dbg3));
  TRY(swdProgramRead32(c, 0x5C001054, &c->dbg4));
  c->debug_saved = true;
  TRY(swdProgramWrite32(c, 0x5C001034, c->dbg3 | 0x40));
  TRY(swdProgramWrite32(c, 0x5C001054, c->dbg4 | 0x40000));
  TRY(swdProgramSetStage(c, "RESET / HALT", 0, 1));
  TRY(swdProgramResetHalt(c));
  /* Reject a core which ran application setup after reset. Never disable a live
   * dirty D-cache through the debug port. A caught system reset gives cold state. */
  TRY(swdProgramRead32(c, 0xE000ED14, &v));
  if (v & ((1U << 16) | (1U << 17)))
  {
    return SWD_ERR_PRECONDITION;
  }
  TRY(swdProgramRead32(c, 0xE000ED94, &v));
  if (v & 1)
  {
    return SWD_ERR_PRECONDITION;
  }
  TRY(swdProgramReadRegister(c, 16, &v));
  if ((v & 0x1FF) || !(v & (1U << 24)))
  {
    return SWD_ERR_PRECONDITION;
  }
  TRY(swdProgramTuneSpeed(c));
  TRY(swdProgramSetStage(c, "LOAD ALGORITHM", 0, c->in->code_size));
  TRY(swdProgramWriteWords(c, 0x24000004, c->in->code, c->in->code_size / 4));
  TRY(swdProgramReadWords(c, 0x24000004, actual, c->in->code_size / 4));
  if (memcmp(c->in->code, actual, c->in->code_size))
  {
    return SWD_ERR_VERIFY;
  }
  TRY(swdProgramWrite32(c, 0x24000000, 0xE7FEBE00));
  TRY(swdProgramRead32(c, 0x24000000, &v));
  if (v != 0xE7FEBE00)
  {
    return SWD_ERR_VERIFY;
  }
  memset(data, 0, sizeof(data));
  for (uint32_t a = STACK_BOTTOM; a < STACK_TOP; a += CHUNK)
  {
    TRY(swdProgramCheckState(c, true, NULL));
    TRY(swdProgramWriteWords(c, a, data, CHUNK / 4));
    swdProgramNotify(c);
  }
  TRY(swdProgramWrite32(c, STACK_BOTTOM, CANARY));
  TRY(swdProgramSetStage(c, "INIT ALGORITHM", 0, 1));
  TRY(swdProgramWrite32(c, FLASH_CCR, FLASH_ERRORS | 0x10000));
  TRY(swdProgramCallLoader(c, 0x2400000B, 0, 0, 0, 0, 10000));
  for (uint32_t i = 0; i < sectors; i++)
  {
    TRY(swdProgramSetStage(c, "ERASE", i, sectors));
    c->r->address = TARGET_FLASH + i * 0x20000;
    TRY(swdProgramWrite32(c, FLASH_CCR, FLASH_ERRORS | 0x10000));
    c->r->erased = true; /* A failed completion can still have erased flash. */
    TRY(swdProgramCallLoader(c, 0x24000399, c->r->address, c->r->address + 0x1FFFF, 3, 0, 30000));
  }
  if (c->erase_only)
  {
    for (uint32_t off = 0; off < 0x100000; off += CHUNK)
    {
      TRY(swdProgramSetStage(c, "BLANK CHECK", off, 0x100000));
      TRY(swdProgramCheckState(c, true, NULL));
      c->r->address = TARGET_FLASH + off;
      TRY(swdProgramReadWords(c, c->r->address, actual, CHUNK / 4));
      for (unsigned i = 0; i < CHUNK / 4; i++)
      {
        if (actual[i] != UINT32_MAX)
        {
          c->r->address += i * 4;
          return SWD_ERR_VERIFY;
        }
      }
      TRY(swdProgramFlashStatus(c));
      c->r->verified = off + CHUNK;
    }
    TRY(swdProgramSetStage(c, "LOCK / HALT", 0x100000, 0x100000));
    TRY(swdProgramRead32(c, FLASH_CR, &v));
    TRY(swdProgramWrite32(c, FLASH_CR, v | 1U));
    TRY(swdProgramRead32(c, FLASH_CR, &v));
    if (!(v & 1U))
    {
      return SWD_ERR_VERIFY;
    }
    TRY(swdProgramCheckState(c, true, NULL));
    c->r->erase_complete = true;
    c->r->stage          = "DONE";
    c->done = c->total = 0x100000;
    swdProgramNotify(c);
    return SWD_OK; /* No reset/resume into the erased vector table. */
  }
  uint32_t crc = UINT32_MAX;
  for (uint32_t off = 0; off < c->in->image_size;)
  {
    TRY(swdProgramSetStage(c, "WRITE", off, c->in->image_size));
    uint32_t n = c->in->image_size - off;
    if (n > CHUNK)
    {
      n = CHUNK;
    }
    uint32_t padded = (n + 31) & ~31U;
    memset(data, 0xFF, padded);
    if (!c->in->read(c->in->context, off, data, n))
    {
      return SWD_ERR_NORESP;
    }
    crc = swdProgramCrcUpdate(crc, (const uint8_t *)data, n);
    TRY(swdProgramCheckState(c, true, NULL));
    TRY(swdProgramWriteWords(c, BUFFER, data, padded / 4));
    /* Check the SWD transfer before giving its bytes to the flash algorithm. */
    TRY(swdProgramReadWords(c, BUFFER, actual, padded / 4));
    if (memcmp(data, actual, padded))
    {
      return SWD_ERR_VERIFY;
    }
    c->r->address = TARGET_FLASH + off;
    TRY(swdProgramWrite32(c, FLASH_CCR, FLASH_ERRORS | 0x10000));
    TRY(swdProgramCallLoader(c, 0x24000041, c->r->address, padded, BUFFER, 3, 5000));
    off           += n;
    c->r->written  = off;
  }
  if (~crc != c->in->image_crc)
  {
    return SWD_ERR_VERIFY;
  }
  crc = UINT32_MAX;
  for (uint32_t off = 0; off < c->in->image_size;)
  {
    TRY(swdProgramSetStage(c, "VERIFY", off, c->in->image_size));
    uint32_t n = c->in->image_size - off;
    if (n > CHUNK)
    {
      n = CHUNK;
    }
    uint32_t padded = (n + 31) & ~31U;
    memset(data, 0xFF, padded);
    if (!c->in->read(c->in->context, off, data, n))
    {
      return SWD_ERR_NORESP;
    }
    crc = swdProgramCrcUpdate(crc, (const uint8_t *)data, n);
    TRY(swdProgramCheckState(c, true, NULL));
    c->r->address = TARGET_FLASH + off;
    TRY(swdProgramReadWords(c, c->r->address, actual, padded / 4));
    for (uint32_t i = 0; i < padded; i++)
    {
      if (((uint8_t *)data)[i] != ((uint8_t *)actual)[i])
      {
        c->r->address += i;
        return SWD_ERR_VERIFY;
      }
    }
    TRY(swdProgramFlashStatus(c));
    off            += n;
    c->r->verified  = off;
  }
  if (~crc != c->in->image_crc)
  {
    return SWD_ERR_VERIFY;
  }
  TRY(swdProgramSetStage(c, "START FIRMWARE", c->in->image_size, c->in->image_size));
  TRY(swdProgramResetHalt(c));
  uint32_t expected_pc;
  TRY(swdProgramRead32(c, TARGET_FLASH + 4, &expected_pc));
  TRY(swdProgramReadRegister(c, 15, &v));
  if (!(expected_pc & 1U) || v != (expected_pc & ~1U))
  {
    return SWD_ERR_PRECONDITION;
  }
  /* Restore debug freeze settings; normal reset has locked the flash again. */
  TRY(swdProgramWrite32(c, 0x5C001034, c->dbg3));
  TRY(swdProgramWrite32(c, 0x5C001054, c->dbg4));
  TRY(swdProgramWrite32(c, DEMCR, 0));
  TRY(swdProgramWrite32(c, DHCSR, KEY | 3));
  TRY(swdProgramWrite32(c, DHCSR, KEY));
  TRY(swdProgramRead32(c, DHCSR, &v));
  if (v & (HALTED | LOCKUP | RESET))
  {
    return SWD_ERR_CORE_STATE;
  }
  c->r->started = true;
  c->r->stage   = "DONE";
  c->done = c->total = c->in->image_size;
  swdProgramNotify(c);
  return SWD_OK;
}

static swd_err_t swdProgramRun(const swd_program_input_t *in, swd_program_result_t *r, bool erase_only)
{
  if (!r)
  {
    return SWD_ERR_ARGUMENT;
  }
  memset(r, 0, sizeof(*r));
  r->stage       = "ARGUMENTS";
  r->link.ap     = 0xFF;
  uint32_t start = millis();
  if (!in || !in->code || in->code_size != 2020 ||
      (!erase_only && (!in->read || !in->image_size || in->image_size > 0x100000)) ||
      ~swdProgramCrcUpdate(UINT32_MAX, (const uint8_t *)in->code, in->code_size) != 0x5AE321FC)
  {
    r->error = SWD_ERR_ARGUMENT;
    return r->error;
  }
  context_t c  = {0};
  c.in         = in;
  c.r          = r;
  c.erase_only = erase_only;
  swdPortBegin(100);
  r->error = swdProgramExecute(&c);
  swdProgramFinishPhase(&c);
  c.recovering = true;
  if (r->error != SWD_OK && c.touched)
  {
    /* Best effort halt only. Never resume a partial image or interrupted loader. */
    swdPortSetSpeed(100);
    (void)swdH723Connect(&r->link);
    (void)swdDapClearErrors(&r->link);
    uint32_t v = 0;
    if (swdProgramRead32(&c, DHCSR, &v) == SWD_OK && swdProgramWrite32(&c, DHCSR, KEY | 3 | (v & 8U)) == SWD_OK)
    {
      uint32_t halt_start = millis();
      do
      {
        if (swdProgramRead32(&c, DHCSR, &v) != SWD_OK)
        {
          break;
        }
        if (v & HALTED)
        {
          r->halted_on_error = true;
          break;
        }
      } while ((uint32_t)(millis() - halt_start) < 500);
    }
  }
  else if (r->error != SWD_OK && c.debug_saved)
  {
    (void)swdProgramWrite32(&c, 0x5C001034, c.dbg3);
    (void)swdProgramWrite32(&c, 0x5C001054, c.dbg4);
  }
  swdRelease();
  r->elapsed_ms = millis() - start;
  return r->error;
}

swd_err_t swdProgramH723(const swd_program_input_t *in, swd_program_result_t *r)
{
  return swdProgramRun(in, r, false);
}

swd_err_t swdEraseH723(const swd_program_input_t *in, swd_program_result_t *r)
{
  return swdProgramRun(in, r, true);
}
