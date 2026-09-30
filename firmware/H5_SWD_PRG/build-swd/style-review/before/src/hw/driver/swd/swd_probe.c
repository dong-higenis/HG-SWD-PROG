/* H723 identification and shared DP/AP access, based on the reference
 * project's posted-read flow. The probe itself only reads target memory. */
#include "swd_transport.h"
#include "swd_dap.h"
#include <string.h>

#ifdef _USE_HW_SWD
#define DP_ABORT       0x00U
#define DP_CTRL_STAT   0x04U
#define DP_SELECT      0x08U
#define DP_RDBUFF      0x0CU
#define AP_CSW         0x00U
#define AP_TAR         0x04U
#define AP_DRW         0x0CU
#define AP_IDR         0xFCU
#define POWER_REQUEST  0x50000000UL
#define POWER_ACK      0xA0000000UL
#define STICKY_ERRORS  0x000000B2UL
#define WAIT_LIMIT     64U

static swd_err_t transfer(swd_probe_t *p, bool ap, bool read, uint8_t addr, uint32_t *data)
{
  for (uint32_t retry = 0; retry < WAIT_LIMIT; retry++)
  {
    p->last_ack = swdWireTransfer(ap, read, addr, data);
    if (p->last_ack == SWD_ACK_OK) return SWD_OK;
    if (p->last_ack != SWD_ACK_WAIT) break;
    p->wait_count++;
  }
  switch (p->last_ack)
  {
    case SWD_ACK_WAIT: return SWD_ERR_WAIT;
    case SWD_ACK_FAULT: return SWD_ERR_FAULT;
    case SWD_ACK_PARITY: return SWD_ERR_PARITY;
    case SWD_ACK_NORESP: return SWD_ERR_NORESP;
    default: return SWD_ERR_PROTOCOL;
  }
}

static swd_err_t dpWrite(swd_probe_t *p, uint8_t addr, uint32_t data)
{
  return transfer(p, false, false, addr, &data);
}

static swd_err_t apSelect(swd_probe_t *p, uint8_t addr)
{
  return dpWrite(p, DP_SELECT, ((uint32_t)p->ap << 24) | (addr & 0xF0U));
}

static swd_err_t apRead(swd_probe_t *p, uint8_t addr, uint32_t *value)
{
  uint32_t stale = 0;
  swd_err_t err = apSelect(p, addr);
  if (err != SWD_OK) return err;
  err = transfer(p, true, true, addr & 0x0CU, &stale);
  if (err != SWD_OK) return err;
  /* AP reads are posted: the first packet's data belongs to an earlier read. */
  return transfer(p, false, true, DP_RDBUFF, value);
}

swd_err_t swdDapClearErrors(swd_probe_t *p)
{
  return dpWrite(p, DP_ABORT, 0x1E);
}

swd_err_t swdMemRead32(swd_probe_t *p, uint32_t addr, uint32_t *value)
{
  if (value == NULL || (addr & 3U)) return SWD_ERR_ARGUMENT;
  uint32_t csw = 0x23000002UL; /* AHB-AP, privileged 32-bit, no increment. */
  swd_err_t err = apSelect(p, AP_CSW);
  if (err != SWD_OK) return err;
  err = transfer(p, true, false, AP_CSW, &csw);
  if (err != SWD_OK) return err;
  err = transfer(p, true, false, AP_TAR, &addr);
  if (err != SWD_OK) return err;
  err = apRead(p, AP_DRW, value);
  if (err != SWD_OK) return err;
  err = transfer(p, false, true, DP_CTRL_STAT, &p->ctrl_stat);
  if (err != SWD_OK) return err;
  return (p->ctrl_stat & STICKY_ERRORS) ? SWD_ERR_FAULT : SWD_OK;
}

swd_err_t swdMemWrite32(swd_probe_t *p, uint32_t addr, uint32_t value)
{
  if (addr & 3U) return SWD_ERR_ARGUMENT;
  uint32_t csw = 0x23000002UL;
  uint32_t completion = 0;
  swd_err_t err = apSelect(p, AP_CSW);
  if (err != SWD_OK) return err;
  err = transfer(p, true, false, AP_CSW, &csw);
  if (err != SWD_OK) return err;
  err = transfer(p, true, false, AP_TAR, &addr);
  if (err != SWD_OK) return err;
  err = transfer(p, true, false, AP_DRW, &value);
  if (err != SWD_OK) return err;
  /* Flush the posted AP write before checking the bus completion status. */
  err = transfer(p, false, true, DP_RDBUFF, &completion);
  if (err != SWD_OK) return err;
  err = transfer(p, false, true, DP_CTRL_STAT, &p->ctrl_stat);
  if (err != SWD_OK) return err;
  return (p->ctrl_stat & STICKY_ERRORS) ? SWD_ERR_FAULT : SWD_OK;
}

static swd_err_t memWords(swd_probe_t *p, uint32_t addr, uint32_t *read_data,
                          const uint32_t *write_data, uint32_t words)
{
  if ((addr & 3U) || (!read_data && !write_data) || words > (UINT32_MAX-addr)/4U)
    return SWD_ERR_ARGUMENT;
  while (words)
  {
    uint32_t count = (1024U - (addr & 1023U))/4U;
    if (count > words) count = words;
    uint32_t csw = 0x23000012UL, value = 0;
    swd_err_t err = apSelect(p, AP_CSW);
    if (err == SWD_OK) err = transfer(p, true, false, AP_CSW, &csw);
    if (err == SWD_OK) err = transfer(p, true, false, AP_TAR, &addr);
    if (err != SWD_OK) return err;
    if (read_data)
    {
      err = transfer(p, true, true, AP_DRW, &value); /* Prime posted read. */
      for (uint32_t i = 0; err == SWD_OK && i+1 < count; i++)
        err = transfer(p, true, true, AP_DRW, &read_data[i]);
      if (err == SWD_OK) err = transfer(p, false, true, DP_RDBUFF, &read_data[count-1]);
      read_data += count;
    }
    else
    {
      for (uint32_t i = 0; err == SWD_OK && i < count; i++)
      {
        value = write_data[i];
        err = transfer(p, true, false, AP_DRW, &value);
      }
      if (err == SWD_OK) err = transfer(p, false, true, DP_RDBUFF, &value);
      write_data += count;
    }
    if (err != SWD_OK) return err;
    err = transfer(p, false, true, DP_CTRL_STAT, &p->ctrl_stat);
    if (err != SWD_OK) return err;
    if (p->ctrl_stat & STICKY_ERRORS) return SWD_ERR_FAULT;
    addr += count*4; words -= count;
  }
  return SWD_OK;
}

swd_err_t swdMemReadWords(swd_probe_t *p, uint32_t addr, uint32_t *data, uint32_t words)
{ return memWords(p, addr, data, NULL, words); }
swd_err_t swdMemWriteWords(swd_probe_t *p, uint32_t addr, const uint32_t *data, uint32_t words)
{ return memWords(p, addr, NULL, data, words); }

swd_err_t swdH723Connect(swd_probe_t *p)
{
  swd_err_t err = SWD_ERR_NORESP;
  p->stage = "DPIDR";
  for (uint32_t attempt = 0; attempt < 3; attempt++)
  {
    swdWireConnect();
    err = transfer(p, false, true, 0, &p->dpidr);
    if (err == SWD_OK) break;
  }
  if (err != SWD_OK) return err;
  if ((p->dpidr & 1U) == 0 || p->dpidr == 0xFFFFFFFFUL) return SWD_ERR_PROTOCOL;

  p->stage = "DP power-up";
  err = dpWrite(p, DP_ABORT, 0x1E); /* Clear stale sticky errors. */
  if (err != SWD_OK) return err;
  err = dpWrite(p, DP_SELECT, 0);
  if (err != SWD_OK) return err;
  err = dpWrite(p, DP_CTRL_STAT, POWER_REQUEST); /* ORUNDETECT stays off. */
  if (err != SWD_OK) return err;
  uint32_t start = millis();
  do
  {
    err = transfer(p, false, true, DP_CTRL_STAT, &p->ctrl_stat);
    if (err != SWD_OK) return err;
    if ((p->ctrl_stat & POWER_ACK) == POWER_ACK) break;
  } while ((uint32_t)(millis() - start) < 100U);
  if ((p->ctrl_stat & POWER_ACK) != POWER_ACK) return SWD_ERR_TIMEOUT;

  swd_err_t last_error = SWD_ERR_NO_AP;
  p->stage = "AP scan";
  for (uint8_t ap = 0; ap < 4; ap++)
  {
    swd_probe_t candidate = *p;
    candidate.ap = ap;
    candidate.ap_idr = candidate.cpuid = candidate.device_id = 0;
    candidate.stage = "AP IDR";
    err = dpWrite(&candidate, DP_ABORT, 0x1E);
    if (err == SWD_OK) err = apRead(&candidate, AP_IDR, &candidate.ap_idr);
    if (err == SWD_OK)
    {
      /* This bring-up supports AHB MEM-APs. Do not apply AHB CSW values
       * to unrelated AP types while scanning the H723 debug fabric. */
      if (((candidate.ap_idr >> 13) & 0xFU) != 8U
          || (candidate.ap_idr & 0xFU) != 1U) continue;
      candidate.stage = "CPUID";
      err = swdMemRead32(&candidate, 0xE000ED00UL, &candidate.cpuid);
      if (err == SWD_OK
          && (candidate.cpuid & 0xFF00FFF0UL) != 0x4100C270UL)
        err = SWD_ERR_MISMATCH;
      if (err == SWD_OK)
      {
        /* ST stm32h723xx.h: DBGMCU_BASE. CubeProgrammer DB 0x483:
         * shared device ID for H72x/H73x, not a full package/order code. */
        candidate.stage = "DBGMCU_IDCODE";
        err = swdMemRead32(&candidate, 0x5C001000UL, &candidate.device_id);
        if (err == SWD_OK)
        {
          *p = candidate;
          if ((p->device_id & 0xFFFU) != 0x483U) return SWD_ERR_MISMATCH;
          p->stage = "complete";
          return SWD_OK;
        }
      }
    }
    *p = candidate;
    last_error = err;
    if (err != SWD_ERR_FAULT && err != SWD_ERR_MISMATCH) return err;
  }
  return last_error;
}

swd_err_t swdProbeH723(uint32_t khz, swd_probe_t *report)
{
  if (report == NULL)
  {
    swdRelease();
    return SWD_ERR_ARGUMENT;
  }
  memset(report, 0, sizeof(*report));
  report->ap = 0xFF;
  report->stage = "arguments";
  swd_err_t err = SWD_ERR_ARGUMENT;
  if (khz >= 10 && khz <= 500)
  {
    swdPortBegin(khz);
    err = swdH723Connect(report);
  }
  swdRelease();
  return err;
}

const char *swdErrStr(swd_err_t err)
{
  switch (err)
  {
    case SWD_OK: return "OK";
    case SWD_ERR_NORESP: return "NO-RESP";
    case SWD_ERR_WAIT: return "WAIT limit";
    case SWD_ERR_FAULT: return "FAULT";
    case SWD_ERR_PARITY: return "PARITY";
    case SWD_ERR_PROTOCOL: return "PROTOCOL";
    case SWD_ERR_TIMEOUT: return "TIMEOUT";
    case SWD_ERR_NO_AP: return "no supported AHB MEM-AP";
    case SWD_ERR_MISMATCH: return "target mismatch";
    case SWD_ERR_ARGUMENT: return "invalid argument";
    case SWD_ERR_VERIFY: return "data mismatch";
    case SWD_ERR_CORE_STATE: return "unexpected core/reset state";
    case SWD_ERR_RESTORE: return "restore failed";
    case SWD_ERR_PRECONDITION: return "execution precondition not met";
    default: return "unknown";
  }
}
#endif
