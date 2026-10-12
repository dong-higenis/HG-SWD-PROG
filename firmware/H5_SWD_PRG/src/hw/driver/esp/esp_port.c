#include "esp_port.h"
#include "hw_def.h"
#include "cli.h"
#include "led.h"
#include "swd.h"
#include <string.h>

/* Shared five-pin connector, ESP mode:
 * CLK/PB4 = UART7 TX; SWO -> non-inverting buffer -> PC1 = software RX.
 * nRESET/PB3 = EN; DIO/PB1 = BOOT, both open drain. PC0 stays input.
 * TIM2 + EXTI1 are reserved while this port is open. No RTOS calls in ISRs.
 */
#define RX_CAPACITY 2048U
#define RX_PIN      GPIO_PIN_1

static UART_HandleTypeDef        tx_uart;
static volatile uint8_t          rx_data[RX_CAPACITY];
static volatile uint32_t         rx_in;
static volatile uint32_t         rx_out;
static volatile esp_port_stats_t stats;
static volatile bool             active;
static unsigned                  rx_bit;
static uint8_t                   rx_byte;
static uint32_t                  bit_ticks;
static uint32_t                  timeout_start;
static uint32_t                  timeout_length;

static void espPortArmStart(void)
{
  TIM2->DIER  = 0;
  EXTI->FPR1  = RX_PIN;
  EXTI->IMR1 |= RX_PIN;
}

void EXTI1_IRQHandler(void)
{
  if (!(EXTI->FPR1 & RX_PIN))
  {
    return;
  }
  EXTI->FPR1 = RX_PIN;
  if (!active || !(EXTI->IMR1 & RX_PIN))
  {
    return;
  }
  EXTI->IMR1 &= ~RX_PIN;
  rx_bit      = 0;
  rx_byte     = 0;
  TIM2->CCR1  = TIM2->CNT + bit_ticks / 2;
  TIM2->SR    = 0;
  TIM2->DIER  = TIM_DIER_CC1IE;
}

void TIM2_IRQHandler(void)
{
  if (!(TIM2->SR & TIM_SR_CC1IF))
  {
    return;
  }
  TIM2->SR = 0;
  if (!active)
  {
    TIM2->DIER = 0;
    return;
  }
  if ((uint32_t)(TIM2->CNT - TIM2->CCR1) > bit_ticks / 3)
  {
    stats.late++;
    espPortArmStart();
    return;
  }
  bool high = (GPIOC->IDR & RX_PIN) != 0;
  if (rx_bit == 0)
  {
    if (high)
    {
      espPortArmStart();
      return;
    }
  }
  else if (rx_bit <= 8)
  {
    if (high)
    {
      rx_byte |= 1U << (rx_bit - 1);
    }
  }
  else
  {
    if (!high)
    {
      stats.framing++;
    }
    else if ((uint32_t)(rx_in - rx_out) == RX_CAPACITY)
    {
      stats.overflow++;
    }
    else
    {
      rx_data[rx_in & (RX_CAPACITY - 1)] = rx_byte;
      __DMB();
      rx_in++;
      stats.received++;
    }
    espPortArmStart();
    return;
  }
  rx_bit++;
  TIM2->CCR1 += bit_ticks;
}

static void espPortFlush(void)
{
  uint32_t saved = __get_PRIMASK();
  __disable_irq();
  rx_in = rx_out = 0;
  stats.framing = stats.overflow = stats.late = 0;
  espPortArmStart();
  __set_PRIMASK(saved);
}

void espPortStats(esp_port_stats_t *out)
{
  out->baud     = stats.baud;
  out->received = stats.received;
  out->framing  = stats.framing;
  out->overflow = stats.overflow;
  out->late     = stats.late;
}

void espPortClose(void)
{
  if (active)
  {
    active = false;
    HAL_NVIC_DisableIRQ(EXTI1_IRQn);
    HAL_NVIC_DisableIRQ(TIM2_IRQn);
    EXTI->IMR1 &= ~RX_PIN;
    EXTI->FPR1  = RX_PIN;
    TIM2->DIER  = 0;
    TIM2->CR1   = 0;
    HAL_NVIC_ClearPendingIRQ(EXTI1_IRQn);
    HAL_NVIC_ClearPendingIRQ(TIM2_IRQn);
    HAL_UART_DeInit(&tx_uart);
    __HAL_RCC_UART7_CLK_DISABLE();
    __HAL_RCC_TIM2_CLK_DISABLE();
    GPIO_InitTypeDef pin = {0};
    pin.Pin              = GPIO_PIN_0 | GPIO_PIN_1;
    pin.Mode             = GPIO_MODE_INPUT;
    pin.Pull             = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &pin);
  }
  swdRelease();
}

static esp_loader_error_t espPortInit(esp_loader_port_t *port)
{
  (void)port;
  memset((void *)&stats, 0, sizeof(stats));
  stats.baud = 115200;
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  swdRelease();
  RCC_PeriphCLKInitTypeDef clock = {0};
  clock.PeriphClockSelection     = RCC_PERIPHCLK_UART7;
  clock.Uart7ClockSelection      = RCC_UART7CLKSOURCE_PCLK1;
  if (HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
  {
    return ESP_LOADER_ERROR_FAIL;
  }
  __HAL_RCC_UART7_CLK_ENABLE();
  __HAL_RCC_TIM2_CLK_ENABLE();
  __HAL_RCC_TIM2_FORCE_RESET();
  __HAL_RCC_TIM2_RELEASE_RESET();
  active               = true;
  uint32_t hclk        = HAL_RCC_GetHCLKFreq();
  uint32_t pclk        = HAL_RCC_GetPCLK1Freq();
  uint32_t mul         = (RCC->CFGR1 & RCC_CFGR1_TIMPRE) ? 4U : 2U;
  uint32_t timer_clock = hclk / pclk <= mul ? hclk : pclk * mul;
  bit_ticks            = (timer_clock + stats.baud / 2) / stats.baud;
  TIM2->PSC            = 0;
  TIM2->ARR            = 0xFFFFFFFF;
  TIM2->EGR            = TIM_EGR_UG;
  TIM2->SR             = 0;
  TIM2->CR1            = TIM_CR1_CEN;

  GPIO_InitTypeDef pin = {0};
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1 | GPIO_PIN_3, GPIO_PIN_SET);
  pin.Pin   = GPIO_PIN_1 | GPIO_PIN_3;
  pin.Mode  = GPIO_MODE_OUTPUT_OD;
  pin.Pull  = GPIO_PULLUP;
  pin.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOB, &pin);
  pin.Pin       = GPIO_PIN_4;
  pin.Mode      = GPIO_MODE_AF_PP;
  pin.Alternate = GPIO_AF11_UART7;
  HAL_GPIO_Init(GPIOB, &pin);
  memset(&tx_uart, 0, sizeof(tx_uart));
  tx_uart.Instance            = UART7;
  tx_uart.Init.BaudRate       = stats.baud;
  tx_uart.Init.WordLength     = UART_WORDLENGTH_8B;
  tx_uart.Init.StopBits       = UART_STOPBITS_1;
  tx_uart.Init.Parity         = UART_PARITY_NONE;
  tx_uart.Init.Mode           = UART_MODE_TX;
  tx_uart.Init.HwFlowCtl      = UART_HWCONTROL_NONE;
  tx_uart.Init.OverSampling   = UART_OVERSAMPLING_16;
  tx_uart.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  if (HAL_UART_Init(&tx_uart) != HAL_OK)
  {
    espPortClose();
    return ESP_LOADER_ERROR_FAIL;
  }
  rx_in = rx_out = 0;
  pin.Pin        = RX_PIN;
  pin.Mode       = GPIO_MODE_IT_FALLING;
  pin.Pull       = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &pin);
  EXTI->FPR1 = RX_PIN;
  HAL_NVIC_SetPriority(EXTI1_IRQn, 1, 0);
  HAL_NVIC_SetPriority(TIM2_IRQn, 1, 0);
  HAL_NVIC_ClearPendingIRQ(EXTI1_IRQn);
  HAL_NVIC_ClearPendingIRQ(TIM2_IRQn);
  HAL_NVIC_EnableIRQ(TIM2_IRQn);
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);
  return ESP_LOADER_SUCCESS;
}

static esp_loader_error_t espPortChangeRate(esp_loader_port_t *port, uint32_t rate)
{
  (void)port;
  if (!active || (rate != 115200 && rate != 230400 && rate != 460800 && rate != 921600))
  {
    return ESP_LOADER_ERROR_INVALID_PARAM;
  }
  HAL_NVIC_DisableIRQ(EXTI1_IRQn);
  HAL_NVIC_DisableIRQ(TIM2_IRQn);
  EXTI->IMR1            &= ~RX_PIN;
  TIM2->DIER             = 0;
  tx_uart.Init.BaudRate  = rate;
  if (HAL_UART_Init(&tx_uart) != HAL_OK)
  {
    return ESP_LOADER_ERROR_FAIL;
  }
  uint32_t hclk        = HAL_RCC_GetHCLKFreq();
  uint32_t pclk        = HAL_RCC_GetPCLK1Freq();
  uint32_t mul         = (RCC->CFGR1 & RCC_CFGR1_TIMPRE) ? 4U : 2U;
  uint32_t timer_clock = hclk / pclk <= mul ? hclk : pclk * mul;
  bit_ticks            = (timer_clock + rate / 2) / rate;
  stats.baud           = rate;
  delay(5);
  TIM2->SR = 0;
  HAL_NVIC_ClearPendingIRQ(EXTI1_IRQn);
  HAL_NVIC_ClearPendingIRQ(TIM2_IRQn);
  espPortFlush();
  HAL_NVIC_EnableIRQ(TIM2_IRQn);
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);
  return ESP_LOADER_SUCCESS;
}

static void espPortBoot(esp_loader_port_t *port)
{
  (void)port;
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1 | GPIO_PIN_3, GPIO_PIN_RESET);
  delay(100);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_SET);
  delay(100);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_SET);
  delay(50);
  espPortFlush();
}

static void espPortReset(esp_loader_port_t *port)
{
  (void)port;
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_RESET);
  delay(100);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_SET);
  delay(100);
}

static void espPortStartTimer(esp_loader_port_t *port, uint32_t ms)
{
  (void)port;
  timeout_start  = millis();
  timeout_length = ms;
}

static uint32_t espPortRemaining(esp_loader_port_t *port)
{
  (void)port;
  uint32_t elapsed = millis() - timeout_start;
  return elapsed >= timeout_length ? 0 : timeout_length - elapsed;
}

static void espPortDelay(esp_loader_port_t *port, uint32_t ms)
{
  (void)port;
  delay(ms);
}

static esp_loader_error_t espPortWrite(esp_loader_port_t *port, const uint8_t *data, uint16_t size, uint32_t timeout)
{
  (void)port;
  ledActivityPulse();
  HAL_StatusTypeDef e = HAL_UART_Transmit(&tx_uart, data, size, timeout);
  return e == HAL_OK ? ESP_LOADER_SUCCESS : e == HAL_TIMEOUT ? ESP_LOADER_ERROR_TIMEOUT : ESP_LOADER_ERROR_FAIL;
}

static esp_loader_error_t espPortRead(esp_loader_port_t *port, uint8_t *data, uint16_t size, uint32_t timeout)
{
  (void)port;
  uint32_t start = millis();
  for (unsigned i = 0; i < size;)
  {
    if (stats.framing || stats.overflow || stats.late)
    {
      return ESP_LOADER_ERROR_INVALID_RESPONSE;
    }
    if (rx_in != rx_out)
    {
      __DMB();
      data[i++] = rx_data[rx_out & (RX_CAPACITY - 1)];
      __DMB();
      rx_out++;
    }
    else if ((uint32_t)(millis() - start) >= timeout)
    {
      return ESP_LOADER_ERROR_TIMEOUT;
    }
    else
    {
      delay(1);
    }
  }
  return ESP_LOADER_SUCCESS;
}

static void espPortLog(esp_loader_port_t *port, esp_loader_log_level_t level, const char *fmt, va_list args)
{
  (void)port;
  (void)level;
  char message[160];
  vsnprintf(message, sizeof(message), fmt, args);
  cliPrintf("ESP      : %s\n", message);
}

static const esp_loader_port_ops_t port_ops = {
  .init                     = espPortInit,
  .change_transmission_rate = espPortChangeRate,
  .enter_bootloader         = espPortBoot,
  .reset_target             = espPortReset,
  .start_timer              = espPortStartTimer,
  .remaining_time           = espPortRemaining,
  .delay_ms                 = espPortDelay,
  .write                    = espPortWrite,
  .read                     = espPortRead,
  .log                      = espPortLog,
};

esp_loader_port_t *espPortGet(void)
{
  static esp_loader_port_t port = {.ops = &port_ops};
  return &port;
}
