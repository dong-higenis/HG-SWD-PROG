#include "swd.h"
#include "swd/swd_transport.h"
#include "cli.h"
#include "led.h"
#include <stdlib.h>
#ifdef _USE_HW_SWD
/* PB1 -> 33R -> target SWDIO; PC0 <- buffer <- target SWDIO.
 * PB4 -> 33R -> target SWCLK; PC1 <- buffer <- target SWO.
 * PB3 -> 33R -> target NRSET. Host PA13/PA14 debug remains available.
 * Target NRST is currently unconnected. Probe leaves the target running. */
volatile uint32_t *swd_tick_counter = &TIM5->CNT;
uint32_t swd_half_ticks = 6;
static bool cycle_counter_ready;

static void pinMode(GPIO_TypeDef *port, uint32_t pins, uint32_t mode)
{
  GPIO_InitTypeDef init = {0};
  init.Pin = pins;
  init.Mode = mode;
  init.Pull = GPIO_NOPULL;
  init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(port, &init);
}

/* Only change our pin's mode. A cached whole MODER word would restore stale
 * SD/UART/I2C settings on this shared GPIO port. Protect the RMW from preemption. */
static void setModeBits(uint32_t mask, uint32_t bits)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  GPIOB->MODER = (GPIOB->MODER & ~mask) | bits;
  __set_PRIMASK(primask);
}

void swdRelease(void)
{
  setModeBits((3UL << 2) | (3UL << 6) | (3UL << 8), 0);
  ledActivityStop();
}
void swdPortActivity(void) { ledActivityPulse(); }
void swdDataOutput(bool enable)
{
  setModeBits(3UL << 2, enable ? (1UL << 2) : 0);
}
void swdDataWrite(bool high) { GPIOB->BSRR = high ? GPIO_PIN_1 : ((uint32_t)GPIO_PIN_1 << 16); }
void swdClockWrite(bool high)
{
  GPIOB->BSRR = high ? GPIO_PIN_4 : ((uint32_t)GPIO_PIN_4 << 16);
}
bool swdDataRead(void) { return (GPIOC->IDR & GPIO_PIN_0) != 0; }
bool swdTraceRead(void) { return HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_1) == GPIO_PIN_SET; }

void swdPortBegin(uint32_t khz)
{
  /* Do not reset CYCCNT: another observer may be using the free-running count.
   * Verify that it advances even in standalone operation without ST-Link. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  __DSB(); __ISB();
  uint32_t before = DWT->CYCCNT;
  for (volatile uint32_t i = 0; i < 32; i++) __NOP();
  cycle_counter_ready = !(DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) && DWT->CYCCNT != before;
  swdPortSetSpeed(khz);
  swdRelease();
  swdDataWrite(true);
  swdClockWrite(true);
  pinMode(GPIOB, GPIO_PIN_1 | GPIO_PIN_4, GPIO_MODE_OUTPUT_PP);
}

uint32_t swdPortMaxKhz(void) { return cycle_counter_ready ? 12000U : 100U; }
void swdPortSetSpeed(uint32_t khz)
{
  if (khz < 10) khz = 10;
  if (khz > swdPortMaxKhz()) khz = swdPortMaxKhz();
  if (cycle_counter_ready)
  {
    swd_tick_counter = &DWT->CYCCNT;
    uint32_t divisor = 2000U * khz;
    swd_half_ticks = (SystemCoreClock + divisor - 1U) / divisor;
  }
  else
  {
    swd_tick_counter = &TIM5->CNT;
    swd_half_ticks = (1000U + 2U * khz - 1U) / (2U * khz) + 1U;
  }
}

void swdPortDelay(void)
{
  swdBitDelay();
}

void swdTargetReset(bool asserted)
{
  if (asserted) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_RESET);
    pinMode(GPIOB, GPIO_PIN_3, GPIO_MODE_OUTPUT_OD);
  } else {
    pinMode(GPIOB, GPIO_PIN_3, GPIO_MODE_INPUT);
  }
}
static void cliSwd(cli_args_t *args)
{
  if (args->argc == 1 && args->isStr(0, "release"))
  {
    swdRelease();
    cliPrintf("SWD outputs released\n");
  }
  else if (args->argc == 1 && args->isStr(0, "info"))
    cliPrintf("SWDIO: PB1 out / PC0 in; SWCLK: PB4; SWO: PC1; NRSET: PB3\n"
              "SWDIO=%d SWO=%d; H723 probe/ramtest/exectest; NRST not connected\n",
              swdDataRead(), swdTraceRead());
  else if ((args->argc == 1 || args->argc == 2)
           && (args->isStr(0, "probe") || args->isStr(0, "ramtest") || args->isStr(0, "exectest")))
  {
    uint32_t khz = 100;
    if (args->argc == 2)
    {
      char *end;
      const char *arg = args->getStr(1);
      unsigned long value = strtoul(arg, &end, 10);
      if (*arg == '\0' || *end != '\0' || value < 10 || value > 500)
      {
        cliPrintf("usage: swd probe|ramtest|exectest [10..500 kHz], default 100\n");
        return;
      }
      khz = (uint32_t)value;
    }
    if (args->isStr(0, "exectest"))
    {
      swd_exectest_t result;
      cliPrintf("H723 execution test: ITCM code, 7 + 35, BX LR -> BKPT\n");
      cliPrintf("Clock ceiling %lu kHz; halt, run test, restore application.\n", (unsigned long)khz);
      swd_err_t err = swdExecTestH723(khz, &result);
      cliPrintf("CODE     : 0x%08lX, ITCMCR=0x%08lX, saved MPU=0x%08lX\n",
                (unsigned long)result.code_addr, (unsigned long)result.itcmcr,
                (unsigned long)result.mpu_ctrl);
      cliPrintf("EXEC     : started=%u, R0=%lu (expected 42)\n",
                result.started, (unsigned long)result.result);
      cliPrintf("STOP     : PC=0x%08lX, DFSR=0x%08lX\n",
                (unsigned long)result.stop_pc, (unsigned long)result.dfsr);
      cliPrintf("TEST     : %s at %s\n", swdErrStr(result.test_status), result.stage);
      cliPrintf("RESTORE  : CODE=%s, REGS=%s, MPU=%s, CORE=%s\n",
                result.code_restored ? "OK" : "FAILED", result.regs_restored ? "OK" : "FAILED",
                result.mpu_restored ? "OK" : "FAILED", result.core_restored ? "OK" : "NOT CONFIRMED");
      cliPrintf("RESULT   : %s (cleanup=%s at %s)\n", swdErrStr(err),
                swdErrStr(result.cleanup_status), result.cleanup_stage);
      if (err != SWD_OK)
        cliPrintf("LINK     : %s, ACK=%lu, DHCSR=0x%08lX\n",
                  result.link.stage != NULL ? result.link.stage : "not started",
                  (unsigned long)result.link.last_ack, (unsigned long)result.dhcsr);
      if (result.cleanup_status != SWD_OK)
        cliPrintf("Recovery incomplete. Check target state before continuing.\n");
      cliPrintf("SWD outputs released. No target reset/flash operation performed.\n");
      return;
    }
    if (args->isStr(0, "ramtest"))
    {
      swd_ramtest_t result;
      cliPrintf("H723 RAM test: halt, save/test/restore 64 bytes, restore core state\n");
      cliPrintf("Clock ceiling %lu kHz; allow a few seconds.\n", (unsigned long)khz);
      swd_err_t err = swdRamTestH723(khz, &result);
      cliPrintf("RAM      : 0x%08lX, %lu bytes, patterns %lu/4\n",
                (unsigned long)result.address, (unsigned long)result.size,
                (unsigned long)result.passes);
      cliPrintf("TEST     : %s at %s\n", swdErrStr(result.test_status), result.stage);
      if (result.fail_addr)
        cliPrintf("MISMATCH : 0x%08lX expected=0x%08lX actual=0x%08lX\n",
                  (unsigned long)result.fail_addr, (unsigned long)result.expected,
                  (unsigned long)result.actual);
      cliPrintf("RESTORE  : RAM=%s, CORE=%s\n",
                !result.ram_written ? "untouched" : result.ram_restored ? "OK" : "FAILED",
                !result.core_saved ? "untouched" : result.core_restored ? "OK" : "NOT CONFIRMED");
      if (result.core_saved)
        cliPrintf("CORE     : before=%s, DHCSR before=0x%08lX after=0x%08lX\n",
                  result.was_halted ? "halted" : "running",
                  (unsigned long)result.dhcsr_before, (unsigned long)result.dhcsr_after);
      cliPrintf("RESULT   : %s (cleanup=%s)\n", swdErrStr(err), swdErrStr(result.cleanup_status));
      if (err != SWD_OK)
        cliPrintf("LINK     : stage=%s, ACK=%lu, WAIT=%lu, CTRL=0x%08lX\n",
                  result.link.stage != NULL ? result.link.stage : "not started",
                  (unsigned long)result.link.last_ack, (unsigned long)result.link.wait_count,
                  (unsigned long)result.link.ctrl_stat);
      if (result.cleanup_status != SWD_OK)
        cliPrintf("Recovery incomplete. Target state must be checked before continuing.\n");
      cliPrintf("SWD outputs released. No target reset/flash operation performed.\n");
      return;
    }
    swd_probe_t report;
    cliPrintf("H723 probe: clock ceiling %lu kHz; target stays running\n", (unsigned long)khz);
    swd_err_t err = swdProbeH723(khz, &report);
    cliPrintf("DPIDR    : 0x%08lX\n", (unsigned long)report.dpidr);
    if (report.ap != 0xFF)
    {
      cliPrintf("AP       : %u, IDR 0x%08lX\n", report.ap, (unsigned long)report.ap_idr);
      cliPrintf("CPUID    : 0x%08lX\n", (unsigned long)report.cpuid);
      cliPrintf("DBGMCU   : 0x%08lX (DEV_ID 0x%03lX)\n",
                (unsigned long)report.device_id, (unsigned long)(report.device_id & 0xFFFU));
    }
    cliPrintf("RESULT   : %s at %s; ACK=%lu, WAIT=%lu, CTRL=0x%08lX\n",
              swdErrStr(err), report.stage, (unsigned long)report.last_ack,
              (unsigned long)report.wait_count, (unsigned long)report.ctrl_stat);
    if (err == SWD_OK)
      cliPrintf("Cortex-M7 / STM32H72x-H73x detected (expected H723ZGT6).\n");
    cliPrintf("SWD outputs released. No target reset/erase/program performed.\n");
  }
  else cliPrintf("swd info\nswd probe [10..500 kHz]\nswd ramtest [10..500 kHz]\nswd exectest [10..500 kHz]\nswd release\n");
}
bool swdInit(void)
{
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  pinMode(GPIOB, GPIO_PIN_1 | GPIO_PIN_3 | GPIO_PIN_4, GPIO_MODE_INPUT);
  swdRelease();
  pinMode(GPIOC, GPIO_PIN_0 | GPIO_PIN_1, GPIO_MODE_INPUT);
  cliAdd("swd", cliSwd);
  return true;
}
#endif
