#include "bsp.h"
#include "hw_def.h"


static void SystemClock_Config(void);
static void PeriphCommonClock_Config(void);
#ifndef _NOT_USE_RTC
static void RtcOscillator_Config(void);
#endif

static DCACHE_HandleTypeDef hdcache1;
volatile bsp_init_stage_t bsp_init_stage = BSP_INIT_RESET;
volatile HAL_StatusTypeDef bsp_init_status = HAL_OK;
volatile HAL_StatusTypeDef bsp_lse_status = HAL_OK;
volatile uint32_t bsp_rtc_clock_source = RCC_RTCCLKSOURCE_NO_CLK;


bool bspInit(void)
{
  bool ret = true;


#ifdef _USE_HW_CACHE
  SCB_EnableICache();
  SCB_EnableDCache();
#endif

  bsp_init_stage = BSP_INIT_HAL;
  bsp_init_status = HAL_Init();
  if (bsp_init_status != HAL_OK) Error_Handler();

  SystemClock_Config();

#ifndef _NOT_USE_RTC
  RtcOscillator_Config();
#endif

  // 클럭 소스 설정
  PeriphCommonClock_Config();

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();

  bsp_init_stage = BSP_INIT_ICACHE;
  bsp_init_status = HAL_ICACHE_Enable();
  if (bsp_init_status != HAL_OK) Error_Handler();

  hdcache1.Instance = DCACHE1;
  hdcache1.Init.ReadBurstType = DCACHE_READ_BURST_WRAP;
  bsp_init_stage = BSP_INIT_DCACHE;
  bsp_init_status = HAL_DCACHE_Init(&hdcache1);
  if (bsp_init_status != HAL_OK)
  {
    Error_Handler();
  }

  bsp_init_stage = BSP_INIT_READY;
  return ret;
}

void delay(uint32_t ms)
{
#ifdef _USE_HW_RTOS
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
    osDelay(ms);
  }
  else
  {
    HAL_Delay(ms);
  }
#else
  HAL_Delay(ms);
#endif
}

uint32_t millis(void)
{
  return HAL_GetTick();
}

void Error_Handler(void)
{
  if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk)
  {
    __BKPT(0);
  }

  __disable_irq();
  while (1)
  {
  }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
  {
  }

  /** Initializes the RCC Oscillators */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState       = RCC_HSE_ON;

  RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource  = RCC_PLL1_SOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM       = 2;
  RCC_OscInitStruct.PLL.PLLN       = 40;
  RCC_OscInitStruct.PLL.PLLP       = 2;
  RCC_OscInitStruct.PLL.PLLQ       = 2;
  RCC_OscInitStruct.PLL.PLLR       = 2;
  RCC_OscInitStruct.PLL.PLLRGE     = RCC_PLL1_VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL  = RCC_PLL1_VCORANGE_WIDE;
  RCC_OscInitStruct.PLL.PLLFRACN   = 0;

  bsp_init_stage = BSP_INIT_OSCILLATORS;
  bsp_init_status = HAL_RCC_OscConfig(&RCC_OscInitStruct);
  if (bsp_init_status != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks */
  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                   | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2
                                   | RCC_CLOCKTYPE_PCLK3;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;

  bsp_init_stage = BSP_INIT_SYSTEM_CLOCK;
  bsp_init_status = HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);
  if (bsp_init_status != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the programming delay */
  __HAL_FLASH_SET_PROGRAM_DELAY(FLASH_PROGRAMMING_DELAY_2);
}

#ifndef _NOT_USE_RTC
static void RtcOscillator_Config(void)
{
  RCC_OscInitTypeDef osc = {0};

  /* Keep HSE/PLL failures separate from the required RTC crystal. */
  HAL_PWR_EnableBkUpAccess();
  osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
  osc.PLL.PLLState = RCC_PLL_NONE;
  bsp_init_stage = BSP_INIT_RTC_LSE;

  /* FC-135 12.5 pF / 70 kohm: use HIGH for board startup validation.
     Stop LSE before changing drive; leave an already configured LSE running. */
  if ((RCC->BDCR & RCC_BDCR_LSEDRV) != RCC_LSEDRIVE_HIGH)
  {
    osc.LSEState = RCC_LSE_OFF;
    bsp_lse_status = HAL_RCC_OscConfig(&osc);
    bsp_init_status = bsp_lse_status;
    if (bsp_lse_status != HAL_OK)
    {
      Error_Handler();
      return;
    }
    __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_HIGH);
  }

  osc.LSEState = RCC_LSE_ON;
  bsp_lse_status = HAL_RCC_OscConfig(&osc);
  bsp_init_status = bsp_lse_status;
  if (bsp_lse_status != HAL_OK)
  {
    Error_Handler();
    return;
  }

  bsp_rtc_clock_source = RCC_RTCCLKSOURCE_LSE;
}
#endif

void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Initializes the peripherals clock */
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_SDMMC1
                                           | RCC_PERIPHCLK_USART1
                                           | RCC_PERIPHCLK_I2C1;

#ifndef _NOT_USE_RTC
  PeriphClkInitStruct.PeriphClockSelection |= RCC_PERIPHCLK_RTC;
  PeriphClkInitStruct.RTCClockSelection     = bsp_rtc_clock_source;
#endif

  PeriphClkInitStruct.PLL2.PLL2Source      = RCC_PLL2_SOURCE_HSE;
  PeriphClkInitStruct.PLL2.PLL2M           = 5;
  PeriphClkInitStruct.PLL2.PLL2N           = 32;
  PeriphClkInitStruct.PLL2.PLL2P           = 3;
  PeriphClkInitStruct.PLL2.PLL2Q           = 2;
  PeriphClkInitStruct.PLL2.PLL2R           = 2;
  PeriphClkInitStruct.PLL2.PLL2RGE         = RCC_PLL2_VCIRANGE_2;
  PeriphClkInitStruct.PLL2.PLL2VCOSEL      = RCC_PLL2_VCORANGE_WIDE;
  PeriphClkInitStruct.PLL2.PLL2FRACN       = 0;
  PeriphClkInitStruct.PLL2.PLL2ClockOut    = RCC_PLL2_DIVR;

  PeriphClkInitStruct.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
  PeriphClkInitStruct.I2c1ClockSelection   = RCC_I2C1CLKSOURCE_PCLK1;
  PeriphClkInitStruct.Sdmmc1ClockSelection = RCC_SDMMC1CLKSOURCE_PLL2R;

  bsp_init_stage = BSP_INIT_PERIPHERAL_CLOCKS;
  bsp_init_status = HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct);
  if (bsp_init_status != HAL_OK)
  {
    Error_Handler();
  }
}
