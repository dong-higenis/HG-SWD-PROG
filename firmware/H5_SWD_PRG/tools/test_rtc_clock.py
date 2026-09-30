"""Verify LSE-only RTC startup and fatal oscillator errors with fault injection."""
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


def function(path, signature):
    text = path.read_text(encoding='utf-8')
    start = text.index(signature + '\n{')
    pos = text.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
enum { BSP_INIT_RTC_LSE };
enum { RCC_OSCILLATORTYPE_LSE=1, RCC_LSE_ON=1, RCC_LSE_OFF=0, RCC_PLL_NONE=0,
       RCC_RTCCLKSOURCE_LSE=0x100, RCC_LSEDRIVE_HIGH=0x18, RCC_BDCR_LSEDRV=0x18 };
static struct { unsigned BDCR; } regs;
#define RCC (&regs)
static unsigned drive_changes;
static void set_drive(unsigned drive) {
  assert((regs.BDCR & 3U) == 0); /* Oscillator must be off before changing drive. */
  drive_changes++;
  regs.BDCR = (regs.BDCR & ~RCC_BDCR_LSEDRV) | drive;
}
#define __HAL_RCC_LSEDRIVE_CONFIG(drive) set_drive(drive)
typedef struct { unsigned OscillatorType, LSEState;
                 struct { unsigned PLLState; } PLL; } RCC_OscInitTypeDef;
static unsigned bsp_init_stage, bsp_rtc_clock_source;
static HAL_StatusTypeDef bsp_init_status, bsp_lse_status;
static HAL_StatusTypeDef first_status, stop_status;
static unsigned calls, errors;
static void HAL_PWR_EnableBkUpAccess(void) {}
static void Error_Handler(void) { errors++; }
static HAL_StatusTypeDef HAL_RCC_OscConfig(RCC_OscInitTypeDef *osc) {
  assert(osc->PLL.PLLState == RCC_PLL_NONE);
  calls++;
  assert(osc->OscillatorType == RCC_OSCILLATORTYPE_LSE);
  if (osc->LSEState == RCC_LSE_OFF) {
    if (stop_status == HAL_OK) regs.BDCR &= ~3U;
    return stop_status;
  }
  assert(osc->LSEState == RCC_LSE_ON);
  assert((regs.BDCR & RCC_BDCR_LSEDRV) == RCC_LSEDRIVE_HIGH);
  regs.BDCR |= first_status == HAL_OK ? 3U : 1U;
  return first_status;
}
#define LSE_VALUE 32768U
#define RTC ((void *)1)
#define RTC_HOURFORMAT_24 0
#define RTC_OUTPUT_DISABLE 0
#define RTC_OUTPUT_POLARITY_HIGH 0
#define RTC_OUTPUT_TYPE_OPENDRAIN 0
#define RTC_OUTPUT_REMAP_NONE 0
typedef struct { void *Instance; struct {
  unsigned HourFormat, AsynchPrediv, SynchPrediv, OutPut,
           OutPutPolarity, OutPutType, OutPutRemap;
} Init; } RTC_HandleTypeDef;
static RTC_HandleTypeDef hrtc;
static bool is_init;
static void logPrintf(const char *fmt, ...) { (void)fmt; }
static HAL_StatusTypeDef HAL_RTC_Init(RTC_HandleTypeDef *rtc) {
  assert(bsp_rtc_clock_source == RCC_RTCCLKSOURCE_LSE);
  assert((rtc->Init.AsynchPrediv+1)*(rtc->Init.SynchPrediv+1) == 32768);
  return HAL_OK;
}
'''

tests = r'''
static void setup(HAL_StatusTypeDef status) {
  calls=errors=0; bsp_rtc_clock_source=0;
  first_status=status;
  stop_status=HAL_OK; drive_changes=0; regs.BDCR=RCC_LSEDRIVE_HIGH | 3U;
}
int main(void) {
  setup(HAL_OK);
  bspRtcOscillatorConfig();
  assert(calls==1 && errors==0 && bsp_rtc_clock_source==RCC_RTCCLKSOURCE_LSE);
  assert(drive_changes==0); /* Preserve an already running HIGH-drive LSE. */
  assert(rtcInit());
  setup(HAL_OK);
  regs.BDCR=3U; /* Previously running at LOW drive. */
  bspRtcOscillatorConfig();
  assert(calls==2 && drive_changes==1 && errors==0);
  assert(bsp_rtc_clock_source==RCC_RTCCLKSOURCE_LSE);
  setup(HAL_OK);
  regs.BDCR=1U; stop_status=HAL_TIMEOUT;
  bspRtcOscillatorConfig();
  assert(calls==1 && drive_changes==0 && errors==1 && bsp_rtc_clock_source==0);
  assert(bsp_init_status==HAL_TIMEOUT);
  setup(HAL_TIMEOUT);
  bspRtcOscillatorConfig();
  assert(calls==1 && errors==1 && bsp_rtc_clock_source==0);
  assert(bsp_lse_status==HAL_TIMEOUT && bsp_init_status==HAL_TIMEOUT);
  assert(bsp_init_stage==BSP_INIT_RTC_LSE);
  setup(HAL_ERROR);
  bspRtcOscillatorConfig();
  assert(calls==1 && errors==1 && bsp_rtc_clock_source==0);
  assert(bsp_init_status==HAL_ERROR);
  setup(HAL_BUSY);
  bspRtcOscillatorConfig();
  assert(calls==1 && errors==1);
  puts("PASS: HIGH drive sequencing, preserve running LSE, 1 Hz divider, fatal errors without fallback");
}
'''

source = preamble + function(root/'src/bsp/bsp.c', 'static void bspRtcOscillatorConfig(void)')
source += '\n' + function(root/'src/hw/driver/rtc.c', 'bool rtcInit(void)') + tests
work = root/'build-swd/host-tests'
work.mkdir(parents=True, exist_ok=True)
cfile, exe = work/'rtc_clock.c', work/'rtc_clock.exe'
cfile.write_text(source, encoding='utf-8')
subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
