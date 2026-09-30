#ifndef SWD_TRANSPORT_H_
#define SWD_TRANSPORT_H_

#include "swd.h"

#define SWD_ACK_OK     1U
#define SWD_ACK_WAIT   2U
#define SWD_ACK_FAULT  4U
#define SWD_ACK_NORESP 7U
#define SWD_ACK_PARITY 8U /* Local error, not a wire ACK. */

void     swdPortBegin(uint32_t khz);
void     swdPortSetSpeed(uint32_t khz);
uint32_t swdPortMaxKhz(void);
void     swdPortDelay(void);
void     swdPortActivity(void);
void     swdWireConnect(void);
void     swdWireIdle(uint32_t cycles);
uint32_t swdWireTransfer(bool ap, bool read, uint8_t addr, uint32_t *data);

/* On the H563, inline the per-bit register operations. Native wire tests keep
 * the same protocol through instrumented port functions. */
#if defined(STM32H563xx)
extern volatile uint32_t *swd_tick_counter;
extern uint32_t           swd_half_ticks;

__STATIC_FORCEINLINE void swdBitDelay(void)
{
  uint32_t start = *swd_tick_counter;
  while ((uint32_t)(*swd_tick_counter - start) < swd_half_ticks)
  {
  }
}

__STATIC_FORCEINLINE void swdBitClock(bool high)
{
  GPIOB->BSRR = high ? GPIO_PIN_4 : ((uint32_t)GPIO_PIN_4 << 16);
}

__STATIC_FORCEINLINE void swdBitSetup(uint32_t bit)
{
  GPIOB->BSRR = ((uint32_t)GPIO_PIN_4 << 16) | ((bit & 1U) ? GPIO_PIN_1 : ((uint32_t)GPIO_PIN_1 << 16));
}

__STATIC_FORCEINLINE uint32_t swdBitSample(void)
{
  return (GPIOC->IDR & GPIO_PIN_0) != 0;
}
#else
static inline void swdBitDelay(void)
{
  swdPortDelay();
}

static inline void swdBitClock(bool high)
{
  swdClockWrite(high);
}

static inline void swdBitSetup(uint32_t bit)
{
  swdClockWrite(false);
  swdDataWrite((bit & 1U) != 0);
}

static inline uint32_t swdBitSample(void)
{
  return swdDataRead() ? 1U : 0U;
}
#endif

#endif
