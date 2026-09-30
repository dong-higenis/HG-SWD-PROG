/* Adapted from Baram/stm32h7-lvgl's GPIO SWD transport.
 * Board I/O is separate: HG-SWD-PROG drives PB1 and samples buffered PC0.
 * One turnaround cycle, LSB first, ORUNDETECT disabled by the probe. */
#include "swd_transport.h"

#ifdef _USE_HW_SWD
static void clockCycle(void)
{
  swdBitClock(false);
  swdBitDelay();
  swdBitClock(true);
  swdBitDelay();
}

static void writeBit(uint32_t bit)
{
  swdBitSetup(bit);
  swdBitDelay();
  swdBitClock(true);
  swdBitDelay();
}

static uint32_t readBit(void)
{
  swdBitClock(false);
  swdBitDelay();
  uint32_t bit = swdBitSample();
  swdBitClock(true);
  swdBitDelay();
  return bit;
}

static uint32_t parity32(uint32_t value)
{
  value ^= value >> 16;
  value ^= value >> 8;
  value ^= value >> 4;
  value ^= value >> 2;
  value ^= value >> 1;
  return value & 1U;
}

void swdWireIdle(uint32_t cycles)
{
  swdDataWrite(false);
  swdDataOutput(true);
  while (cycles--) clockCycle();
  swdDataWrite(true);
}

static void lineReset(void)
{
  swdDataWrite(true);
  swdDataOutput(true);
  for (uint32_t i = 0; i < 56; i++) clockCycle();
}

void swdWireConnect(void)
{
  lineReset();
  for (uint32_t i = 0; i < 16; i++) writeBit(0xE79EU >> i);
  lineReset();
  swdWireIdle(8);
}

uint32_t swdWireTransfer(bool ap, bool read, uint8_t addr, uint32_t *data)
{
  swdPortActivity();
  uint32_t payload = ((uint32_t)ap << 1) | ((uint32_t)read << 2)
                   | ((addr & 0x0CU) << 1);
  uint32_t request = 0x81U | payload | (parity32(payload) << 5);
  uint32_t ack = 0;

  swdDataOutput(true);
  for (uint32_t i = 0; i < 8; i++) writeBit(request >> i);
  swdDataOutput(false);
  clockCycle();
  for (uint32_t i = 0; i < 3; i++) ack |= readBit() << i;

  if (ack == SWD_ACK_OK)
  {
    if (read)
    {
      uint32_t value = 0;
      for (uint32_t i = 0; i < 32; i++) value |= readBit() << i;
      uint32_t parity = readBit();
      clockCycle();
      swdDataWrite(true);
      swdDataOutput(true);
      if (parity != parity32(value)) ack = SWD_ACK_PARITY;
      else if (data != NULL) *data = value;
    }
    else
    {
      clockCycle();
      swdDataOutput(true);
      uint32_t value = data != NULL ? *data : 0;
      for (uint32_t i = 0; i < 32; i++) writeBit(value >> i);
      writeBit(parity32(value));
    }
    swdWireIdle(8);
  }
  else
  {
    /* WAIT/FAULT have no data phase when ORUNDETECT=0. For an invalid
     * ACK, drain a possible data phase before taking ownership back. */
    uint32_t cycles = (ack == SWD_ACK_WAIT || ack == SWD_ACK_FAULT) ? 1 : 34;
    while (cycles--) clockCycle();
    swdDataWrite(true);
    swdDataOutput(true);
    swdWireIdle(8);
  }
  return ack;
}
#endif
