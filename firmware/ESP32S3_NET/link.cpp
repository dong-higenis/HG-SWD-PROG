#include "link.h"

#include "config.h"
#include "crc16.h"
#include "log.h"
#include "protocol.h"
#include "protocol_defs.h"

namespace
{
constexpr uint32_t INTER_BYTE_TIMEOUT_MS = 50;
constexpr size_t   HEADER_LEN            = 6;  // sync(2) cmd seq len(2)

enum class RxState : uint8_t
{
  SYNC1,
  SYNC2,
  CMD,
  SEQ,
  LEN_LO,
  LEN_HI,
  PAYLOAD,
  CRC_LO,
  CRC_HI,
};

HardwareSerial &port = Serial2;

RxState  rx_state   = RxState::SYNC1;
uint8_t  rx_cmd     = 0;
uint8_t  rx_seq     = 0;
uint16_t rx_len     = 0;
uint16_t rx_pos     = 0;
uint16_t rx_crc     = 0;
uint8_t  rx_crc_lo  = 0;
uint32_t rx_last_ms = 0;
uint8_t  rx_payload[FRAME_MAX_PAYLOAD];

// Retransmit cache: the last processed request (cmd, seq) and the exact response
// frame sent for it. A repeat of the same cmd+seq is answered from here without
// re-executing the command.
bool     cache_valid = false;
uint8_t  cache_cmd   = 0;
uint8_t  cache_seq   = 0;
uint8_t  tx_frame[FRAME_MAX_PAYLOAD + FRAME_OVERHEAD];
size_t   tx_len      = 0;

LinkStats stats    = {};
bool      active   = false;

void resetParser(void)
{
  rx_state = RxState::SYNC1;
}

void sendCached(void)
{
  port.write(tx_frame, tx_len);
}

void handleFrame(void)
{
  stats.frames++;

  if (rx_cmd & FRAME_RESP_FLAG)
  {
    // Response-range cmd from the master side: not a request, ignore.
    return;
  }

  if (cache_valid && rx_cmd == cache_cmd && rx_seq == cache_seq)
  {
    stats.duplicates++;
    LOGV("LINK", "dup cmd=%02X seq=%u -> cached", rx_cmd, rx_seq);
    sendCached();
    return;
  }

  uint8_t *resp     = &tx_frame[HEADER_LEN];
  size_t   resp_len = protocolHandle(rx_cmd, rx_payload, rx_len, resp, FRAME_MAX_PAYLOAD);

  if (resp_len == 0 || resp_len > FRAME_MAX_PAYLOAD)
  {
    resp[0]  = ST_ERROR;
    resp_len = 1;
  }

  tx_frame[0] = FRAME_SYNC1;
  tx_frame[1] = FRAME_SYNC2;
  tx_frame[2] = rx_cmd | FRAME_RESP_FLAG;
  tx_frame[3] = rx_seq;
  tx_frame[4] = (uint8_t)(resp_len & 0xFF);
  tx_frame[5] = (uint8_t)(resp_len >> 8);

  uint16_t crc = crc16Ccitt(&tx_frame[2], 4 + resp_len);
  tx_frame[HEADER_LEN + resp_len]     = (uint8_t)(crc & 0xFF);
  tx_frame[HEADER_LEN + resp_len + 1] = (uint8_t)(crc >> 8);
  tx_len = HEADER_LEN + resp_len + 2;

  cache_valid = true;
  cache_cmd   = rx_cmd;
  cache_seq   = rx_seq;

  LOGV("LINK", "cmd=%02X seq=%u len=%u -> st=%u len=%u", rx_cmd, rx_seq, rx_len, resp[0], (unsigned)resp_len);
  sendCached();
}

void feed(uint8_t b)
{
  switch (rx_state)
  {
    case RxState::SYNC1:
      if (b == FRAME_SYNC1)
      {
        rx_state = RxState::SYNC2;
      }
      break;

    case RxState::SYNC2:
      if (b == FRAME_SYNC2)
      {
        rx_state = RxState::CMD;
      }
      else if (b != FRAME_SYNC1)  // "A5 A5 5A" still syncs
      {
        rx_state = RxState::SYNC1;
      }
      break;

    case RxState::CMD:
      rx_cmd   = b;
      rx_crc   = crc16Update(0xFFFF, b);
      rx_state = RxState::SEQ;
      break;

    case RxState::SEQ:
      rx_seq   = b;
      rx_crc   = crc16Update(rx_crc, b);
      rx_state = RxState::LEN_LO;
      break;

    case RxState::LEN_LO:
      rx_len   = b;
      rx_crc   = crc16Update(rx_crc, b);
      rx_state = RxState::LEN_HI;
      break;

    case RxState::LEN_HI:
      rx_len |= (uint16_t)b << 8;
      rx_crc  = crc16Update(rx_crc, b);
      if (rx_len > FRAME_MAX_PAYLOAD)
      {
        stats.oversize++;
        resetParser();
        break;
      }
      rx_pos   = 0;
      rx_state = rx_len ? RxState::PAYLOAD : RxState::CRC_LO;
      break;

    case RxState::PAYLOAD:
      rx_payload[rx_pos++] = b;
      rx_crc               = crc16Update(rx_crc, b);
      if (rx_pos >= rx_len)
      {
        rx_state = RxState::CRC_LO;
      }
      break;

    case RxState::CRC_LO:
      rx_crc_lo = b;
      rx_state  = RxState::CRC_HI;
      break;

    case RxState::CRC_HI:
    {
      uint16_t rx_frame_crc = (uint16_t)rx_crc_lo | ((uint16_t)b << 8);
      resetParser();
      if (rx_frame_crc != rx_crc)
      {
        stats.crc_errors++;
        LOGV("LINK", "crc error cmd=%02X seq=%u", rx_cmd, rx_seq);
        break;  // dropped silently; the master retransmits
      }
      handleFrame();
    }
    break;
  }
}
}  // namespace

void linkInit(void)
{
  port.setRxBufferSize(H5_RX_BUF_SIZE);
  port.setTxBufferSize(H5_TX_BUF_SIZE);
  port.begin(H5_BAUD, SERIAL_8N1, H5_RX_PIN, H5_TX_PIN);
  resetParser();
}

void linkPoll(void)
{
  active = false;

  while (port.available() > 0)
  {
    uint32_t now = millis();
    int      c   = port.read();
    if (c < 0)
    {
      break;
    }

    if (rx_state != RxState::SYNC1 && now - rx_last_ms > INTER_BYTE_TIMEOUT_MS)
    {
      stats.timeouts++;
      resetParser();
    }
    rx_last_ms = now;
    active     = true;
    feed((uint8_t)c);
  }

  // A stale partial frame is dropped even if no further byte arrives.
  if (rx_state != RxState::SYNC1 && millis() - rx_last_ms > INTER_BYTE_TIMEOUT_MS)
  {
    stats.timeouts++;
    resetParser();
  }
}

bool linkActive(void)
{
  return active;
}

void linkGetStats(LinkStats *out)
{
  *out = stats;
}
