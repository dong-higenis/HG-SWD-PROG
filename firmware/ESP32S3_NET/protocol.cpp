#include "protocol.h"

#include <Arduino.h>

#include "config.h"
#include "net.h"
#include "net_http.h"
#include "net_wifi.h"
#include "protocol_defs.h"

// Commands without a defined request payload ignore any bytes sent with them, so a
// future master may append optional fields. Commands with a payload are checked strictly.

namespace
{
constexpr size_t REASON_MAX = 63;

size_t reply(uint8_t *out, uint8_t status)
{
  out[0] = status;
  return 1;
}

size_t replyReason(uint8_t *out, uint8_t status, const char *reason)
{
  out[0]     = status;
  size_t len = reason ? strnlen(reason, REASON_MAX) : 0;
  memcpy(&out[1], reason, len);
  return 1 + len;
}

// Copies a length-prefixed text field; rejects NUL and control characters.
bool copyText(const uint8_t *src, size_t len, char *dst, bool allow_quote)
{
  for (size_t i = 0; i < len; i++)
  {
    uint8_t c = src[i];
    if (c < 0x20 || c == 0x7F || (!allow_quote && c == '"'))
    {
      return false;
    }
    dst[i] = (char)c;
  }
  dst[len] = '\0';
  return true;
}

size_t cmdHello(uint8_t *out)
{
  static const char version[] = NET_FW_NAME " " NET_FW_VERSION;
  out[0] = ST_OK;
  out[1] = PROTO_VERSION;
  memcpy(&out[2], version, sizeof(version) - 1);
  return 2 + sizeof(version) - 1;
}

size_t cmdStatus(uint8_t *out)
{
  NetStatusInfo st;
  netGetStatus(&st);
  out[0] = ST_OK;
  out[1] = st.mode;
  out[2] = st.state;
  out[3] = (uint8_t)st.rssi;
  memcpy(&out[4], st.ip, 4);
  size_t len = strnlen(st.detail, STATUS_DETAIL_MAX);
  memcpy(&out[8], st.detail, len);
  return 8 + len;
}

size_t cmdWifiList(uint8_t *out, size_t cap)
{
  switch (wifiScanState())
  {
    case APSCAN_BUSY:  return reply(out, ST_BUSY);
    case APSCAN_ERROR: return replyReason(out, ST_ERROR, "scan failed");
    default:           break;
  }

  uint8_t            count = 0;
  const WifiApEntry *aps   = wifiScanResults(&count);
  size_t             pos   = 2;
  uint8_t            n     = 0;
  for (uint8_t i = 0; i < count; i++)
  {
    size_t need = 3 + aps[i].ssid_len;
    if (pos + need > cap)
    {
      break;  // cannot happen with 16 x 35 bytes, kept as a guard
    }
    out[pos++] = (uint8_t)aps[i].rssi;
    out[pos++] = aps[i].auth;
    out[pos++] = aps[i].ssid_len;
    memcpy(&out[pos], aps[i].ssid, aps[i].ssid_len);
    pos += aps[i].ssid_len;
    n++;
  }
  out[0] = ST_OK;
  out[1] = n;
  return pos;
}

size_t cmdWifiJoin(const uint8_t *req, uint16_t len, uint8_t *out)
{
  char ssid[WIFI_SSID_MAX + 1];
  char pw[WIFI_PW_MAX + 1];

  if (len < 2)
  {
    return reply(out, ST_BAD_REQUEST);
  }
  size_t ssid_len = req[0];
  if (ssid_len < 1 || ssid_len > WIFI_SSID_MAX || len < 2 + ssid_len)
  {
    return reply(out, ST_BAD_REQUEST);
  }
  size_t pw_len = req[1 + ssid_len];
  if (pw_len > WIFI_PW_MAX || len != 2 + ssid_len + pw_len)
  {
    return reply(out, ST_BAD_REQUEST);
  }
  // SSIDs are raw bytes; only NUL is refused because the driver API takes C strings.
  if (memchr(&req[1], 0, ssid_len) != nullptr || !copyText(&req[2 + ssid_len], pw_len, pw, true))
  {
    return reply(out, ST_BAD_REQUEST);
  }
  memcpy(ssid, &req[1], ssid_len);
  ssid[ssid_len] = '\0';

  return reply(out, netWifiJoin(ssid, pw));
}

size_t cmdLteJoin(const uint8_t *req, uint16_t len, uint8_t *out)
{
  char apn[LTE_APN_MAX + 1];
  if (len < 1)
  {
    return reply(out, ST_BAD_REQUEST);
  }
  size_t apn_len = req[0];
  if (apn_len > LTE_APN_MAX || len != 1 + apn_len || !copyText(&req[1], apn_len, apn, false))
  {
    return reply(out, ST_BAD_REQUEST);  // '"' would break AT+CGDCONT quoting
  }
  return reply(out, netLteJoin(apn));
}

size_t cmdHttpGet(const uint8_t *req, uint16_t len, uint8_t *out)
{
  char url[HTTP_URL_MAX + 1];
  if (len < 2)
  {
    return reply(out, ST_BAD_REQUEST);
  }
  size_t url_len = (size_t)req[0] | ((size_t)req[1] << 8);
  if (url_len < 1 || url_len > HTTP_URL_MAX || len != 2 + url_len || !copyText(&req[2], url_len, url, true))
  {
    return reply(out, ST_BAD_REQUEST);
  }

  uint16_t code   = 0;
  uint32_t length = 0;
  char     reason[REASON_MAX + 1] = "";
  uint8_t  st     = httpGet(url, &code, &length, reason, sizeof(reason));
  if (st != ST_OK)
  {
    return replyReason(out, st, reason);
  }
  out[0] = ST_OK;
  out[1] = (uint8_t)(code & 0xFF);
  out[2] = (uint8_t)(code >> 8);
  out[3] = (uint8_t)(length & 0xFF);
  out[4] = (uint8_t)(length >> 8);
  out[5] = (uint8_t)(length >> 16);
  out[6] = (uint8_t)(length >> 24);
  return 7;
}

size_t cmdHttpRead(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
  if (len != 2)
  {
    return reply(out, ST_BAD_REQUEST);
  }
  uint16_t max = (uint16_t)(req[0] | (req[1] << 8));
  if (max < 1 || max > HTTP_READ_MAX || (size_t)max + 1 > cap)
  {
    return reply(out, ST_BAD_REQUEST);
  }

  uint16_t got = 0;
  char     reason[REASON_MAX + 1] = "";
  uint8_t  st  = httpRead(&out[1], max, &got, reason, sizeof(reason));
  if (st != ST_OK)
  {
    return replyReason(out, st, reason);
  }
  out[0] = ST_OK;
  return 1 + got;
}
}  // namespace

size_t protocolHandle(uint8_t cmd, const uint8_t *req, uint16_t req_len, uint8_t *out, size_t out_cap)
{
  switch (cmd)
  {
    case CMD_HELLO:      return cmdHello(out);
    case CMD_STATUS:     return cmdStatus(out);
    case CMD_OFF:        return reply(out, netOff());
    case CMD_WIFI_SCAN:  return reply(out, netWifiScan());
    case CMD_WIFI_LIST:  return cmdWifiList(out, out_cap);
    case CMD_WIFI_JOIN:  return cmdWifiJoin(req, req_len, out);
    case CMD_LTE_JOIN:   return cmdLteJoin(req, req_len, out);
    case CMD_HTTP_GET:   return cmdHttpGet(req, req_len, out);
    case CMD_HTTP_READ:  return cmdHttpRead(req, req_len, out, out_cap);
    case CMD_HTTP_CLOSE: httpClose(); return reply(out, ST_OK);
    default:             return reply(out, ST_UNKNOWN_CMD);
  }
}
