#include "net_link.h"
#include <string.h>

static const net_link_io_t *link_io;
static uint8_t              link_seq;
static uint8_t              tx_frame[NET_LINK_FRAME_MAX];
static net_frame_rx_t       rx;

uint16_t netLinkCrc(const uint8_t *data, uint32_t size)
{
  uint16_t crc = 0xFFFF; /* CRC-16/CCITT-FALSE */
  for (uint32_t i = 0; i < size; i++)
  {
    crc ^= (uint16_t)data[i] << 8;
    for (unsigned bit = 0; bit < 8; bit++)
    {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

uint32_t netLinkEncode(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t size, uint8_t *frame)
{
  frame[0] = 0xA5;
  frame[1] = 0x5A;
  frame[2] = cmd;
  frame[3] = seq;
  frame[4] = (uint8_t)size;
  frame[5] = (uint8_t)(size >> 8);
  if (size)
  {
    memcpy(frame + 6, payload, size);
  }
  uint16_t crc     = netLinkCrc(frame + 2, 4U + size);
  frame[6 + size]  = (uint8_t)crc;
  frame[7 + size]  = (uint8_t)(crc >> 8);
  return 8U + size;
}

void netLinkRxReset(net_frame_rx_t *r)
{
  r->pos = 0;
  r->len = 0;
}

bool netLinkRxFeed(net_frame_rx_t *r, uint8_t byte)
{
  if ((r->pos == 0 && byte != 0xA5) || (r->pos == 1 && byte != 0x5A))
  {
    r->pos = byte == 0xA5 ? 1 : 0; /* Resynchronise on the next start byte. */
    r->frame[0] = 0xA5;
    return false;
  }
  r->frame[r->pos++] = byte;
  if (r->pos == 6)
  {
    r->len = (uint16_t)(r->frame[4] | (r->frame[5] << 8));
    if (r->len > NET_LINK_PAYLOAD_MAX)
    {
      netLinkRxReset(r);
    }
    return false;
  }
  if (r->pos < 6 || r->pos < 8U + r->len)
  {
    return false;
  }
  uint16_t crc = (uint16_t)(r->frame[6 + r->len] | (r->frame[7 + r->len] << 8));
  bool     ok  = crc == netLinkCrc(r->frame + 2, 4U + r->len);
  r->pos       = 0;
  return ok;
}

void netLinkBind(const net_link_io_t *io)
{
  link_io = io;
}

void netLinkSeed(uint8_t seq)
{
  link_seq = seq;
}

uint8_t netLinkCall(uint8_t cmd, const void *request, uint16_t request_size, void *data, uint16_t data_max,
                    uint16_t *size, uint32_t timeout_ms, unsigned attempts)
{
  if (size)
  {
    *size = 0;
  }
  if (!link_io || request_size > NET_LINK_PAYLOAD_MAX)
  {
    return NET_ST_NO_LINK;
  }
  link_seq++;
  uint32_t length = netLinkEncode(cmd, link_seq, request, request_size, tx_frame);
  for (unsigned attempt = 0; attempt < attempts; attempt++)
  {
    while (link_io->available())
    {
      link_io->read(); /* Late replies to an earlier attempt. */
    }
    netLinkRxReset(&rx);
    if (link_io->write(tx_frame, length) != length)
    {
      continue;
    }
    uint32_t start = link_io->millis();
    while ((uint32_t)(link_io->millis() - start) < timeout_ms)
    {
      if (!link_io->available())
      {
        link_io->idle();
        continue;
      }
      if (!netLinkRxFeed(&rx, link_io->read()))
      {
        continue;
      }
      uint16_t n = (uint16_t)(rx.frame[4] | (rx.frame[5] << 8));
      if (rx.frame[2] != (cmd | 0x80) || rx.frame[3] != link_seq || n == 0)
      {
        continue; /* Not ours; keep listening. */
      }
      uint16_t copy = n - 1U < data_max ? n - 1U : data_max;
      if (data && copy)
      {
        memcpy(data, rx.frame + 7, copy);
      }
      if (size)
      {
        *size = copy;
      }
      return rx.frame[6];
    }
  }
  return NET_ST_NO_LINK;
}
