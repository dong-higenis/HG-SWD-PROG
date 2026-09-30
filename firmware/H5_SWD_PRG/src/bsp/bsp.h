#ifndef BSP_H_
#define BSP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "def.h"
#include "stm32h5xx_hal.h"
#include "rtos.h"



void logPrintf(const char *fmt, ...);



/* Inspect these in GDB when Error_Handler stops during board startup. */
typedef enum
{
  BSP_INIT_RESET = 0,
  BSP_INIT_HAL,
  BSP_INIT_OSCILLATORS,
  BSP_INIT_SYSTEM_CLOCK,
  BSP_INIT_PERIPHERAL_CLOCKS,
  BSP_INIT_ICACHE,
  BSP_INIT_DCACHE,
  BSP_INIT_READY,
  BSP_INIT_RTC_LSE
} bsp_init_stage_t;
extern volatile bsp_init_stage_t bsp_init_stage;
extern volatile HAL_StatusTypeDef bsp_init_status;
extern volatile HAL_StatusTypeDef bsp_lse_status;
extern volatile uint32_t bsp_rtc_clock_source;

bool bspInit(void);

void delay(uint32_t time_ms);
uint32_t millis(void);
uint32_t micros(void);

void Error_Handler(void);


#ifdef __cplusplus
}
#endif

#endif
