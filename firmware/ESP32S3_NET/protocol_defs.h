#ifndef PROTOCOL_DEFS_H_
#define PROTOCOL_DEFS_H_

#include <stddef.h>
#include <stdint.h>

// Wire protocol constants. Must match PROTOCOL.md (v1).

constexpr uint8_t  PROTO_VERSION     = 1;
constexpr uint8_t  FRAME_SYNC1       = 0xA5;
constexpr uint8_t  FRAME_SYNC2       = 0x5A;
constexpr uint16_t FRAME_MAX_PAYLOAD = 1100;
constexpr size_t   FRAME_OVERHEAD    = 8;  // sync(2) cmd seq len(2) crc(2)
constexpr uint8_t  FRAME_RESP_FLAG   = 0x80;

enum NetStatusCode : uint8_t
{
  ST_OK            = 0,
  ST_BUSY          = 1,
  ST_ERROR         = 2,
  ST_BAD_REQUEST   = 3,
  ST_NOT_CONNECTED = 4,
  ST_HTTP          = 5,
  ST_TIMEOUT       = 6,
  ST_UNKNOWN_CMD   = 7,
};

enum NetCmd : uint8_t
{
  CMD_HELLO      = 0x01,
  CMD_STATUS     = 0x02,
  CMD_OFF        = 0x03,
  CMD_WIFI_SCAN  = 0x10,
  CMD_WIFI_LIST  = 0x11,
  CMD_WIFI_JOIN  = 0x12,
  CMD_LTE_JOIN   = 0x20,
  CMD_HTTP_GET   = 0x30,
  CMD_HTTP_READ  = 0x31,
  CMD_HTTP_CLOSE = 0x32,
};

// STATUS fields
enum NetMode : uint8_t
{
  MODE_OFF  = 0,
  MODE_WIFI = 1,
  MODE_LTE  = 2,
};

enum NetState : uint8_t
{
  STATE_IDLE       = 0,
  STATE_CONNECTING = 1,
  STATE_CONNECTED  = 2,
  STATE_FAILED     = 3,
};

constexpr size_t STATUS_DETAIL_MAX = 47;
constexpr size_t WIFI_SSID_MAX     = 32;
constexpr size_t WIFI_PW_MAX       = 64;
constexpr size_t LTE_APN_MAX       = 63;

#endif
