#ifndef CRC16_H_
#define CRC16_H_

#include <stddef.h>
#include <stdint.h>

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR.
constexpr uint16_t crc16Update(uint16_t crc, uint8_t data)
{
  crc ^= (uint16_t)data << 8;
  for (int i = 0; i < 8; i++)
  {
    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

constexpr uint16_t crc16Ccitt(const uint8_t *data, size_t len, uint16_t crc = 0xFFFF)
{
  for (size_t i = 0; i < len; i++)
  {
    crc = crc16Update(crc, data[i]);
  }
  return crc;
}

constexpr uint16_t crc16CcittStr(const char *s, uint16_t crc = 0xFFFF)
{
  while (*s)
  {
    crc = crc16Update(crc, (uint8_t)*s++);
  }
  return crc;
}

// Standard check value for the catalogue entry "CRC-16/IBM-3740" (a.k.a. CCITT-FALSE).
static_assert(crc16CcittStr("123456789") == 0x29B1, "CRC-16/CCITT-FALSE check value mismatch");

#endif
