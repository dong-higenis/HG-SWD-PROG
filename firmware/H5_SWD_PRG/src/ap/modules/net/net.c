#include "module.h"
#include "net.h"
#include <string.h>

#define NET_TIMEOUT_MS      300
#define NET_HTTP_GET_MS     25000
#define NET_HTTP_READ_MS    8000
#define NET_ATTEMPTS        3
/* Background checks run from the UI loop, so each is one short try: an ESP that is missing or miswired
 * costs at most NET_PROBE_TIMEOUT every NET_PROBE_MS, never a stalled screen. */
#define NET_PROBE_MS        3000 /* HELLO period while the ESP is missing. */
#define NET_PROBE_TIMEOUT   20
#define NET_STATUS_MS       1000 /* STATUS period while it answers. */
#define NET_STATUS_TIMEOUT  100
#define NET_PRESENT_MS      10000

const net_apn_t net_apns[] = {
  {"Global", "mobile"},
  {"SKT", "lte.sktelecom.com"},
  {"SKT IoT", "iot.sktelecom.com"},
  {"KT", "lte.ktfwing.com"},
  {"KT IoT", "iot.ktfwing.com"},
  {"LG U+", "internet.lguplus.co.kr"},
  {"LG U+ IoT", "iot.lguplus.co.kr"},
};
const unsigned net_apn_count = sizeof(net_apns) / sizeof(net_apns[0]);

static struct
{
  bool        present;
  uint32_t    seen; /* millis() of the last valid response. */
  uint32_t    probe;
  bool        rejoin; /* Saved network still to be rejoined. */
  bool        mounted;
  char        version[32];
  net_info_t  info;
  net_store_t settings;
} net;

static uint32_t netIoAvailable(void)
{
  return uartAvailable(HW_UART_CH_NET);
}

static uint8_t netIoRead(void)
{
  return uartRead(HW_UART_CH_NET);
}

static uint32_t netIoWrite(const uint8_t *data, uint32_t size)
{
  return uartWrite(HW_UART_CH_NET, (uint8_t *)data, size);
}

static void (*idle_hook)(void);

void netSetIdleHook(void (*hook)(void))
{
  idle_hook = hook;
}

static void netIoIdle(void)
{
  delay(1);
  if (idle_hook)
  {
    idle_hook();
  }
}

static const net_link_io_t net_io = {netIoAvailable, netIoRead, netIoWrite, millis, netIoIdle};

static uint8_t netCall(uint8_t cmd, const void *request, uint16_t size, void *data, uint16_t max, uint16_t *got,
                       uint32_t timeout_ms)
{
  uint8_t status = netLinkCall(cmd, request, size, data, max, got, timeout_ms, NET_ATTEMPTS);
  if (status != NET_ST_NO_LINK)
  {
    net.present = true;
    net.seen    = millis();
  }
  else
  {
    net.present = false;
  }
  return status;
}

bool netIsPresent(void)
{
  return net.present && (uint32_t)(millis() - net.seen) < NET_PRESENT_MS;
}

const char *netEspVersion(void)
{
  return net.version;
}

/* HELLO reply: protocol version, then the ESP firmware version text. */
static void netHelloParse(uint8_t *data, uint16_t got)
{
  data[got] = 0;
  snprintf(net.version, sizeof(net.version), "p%u %.24s", got ? data[0] : 0, got ? (const char *)data + 1 : "");
}

uint8_t netHello(void)
{
  uint8_t  data[40];
  uint16_t got    = 0;
  uint8_t  status = netCall(NET_CMD_HELLO, NULL, 0, data, sizeof(data) - 1, &got, NET_TIMEOUT_MS);
  if (status == NET_ST_OK)
  {
    netHelloParse(data, got);
  }
  return status;
}

static uint8_t netInfoCall(net_info_t *info, uint32_t timeout_ms, unsigned attempts)
{
  uint8_t  data[64];
  uint16_t got    = 0;
  uint8_t  status = netLinkCall(NET_CMD_STATUS, NULL, 0, data, sizeof(data) - 1, &got, timeout_ms, attempts);
  net.present     = status != NET_ST_NO_LINK;
  if (net.present)
  {
    net.seen = millis();
  }
  memset(&net.info, 0, sizeof(net.info));
  if (status == NET_ST_OK && got >= 7)
  {
    net.info.mode  = data[0];
    net.info.state = data[1];
    net.info.rssi  = (int8_t)data[2];
    memcpy(net.info.ip, data + 3, 4);
    data[got] = 0;
    snprintf(net.info.detail, sizeof(net.info.detail), "%.47s", (const char *)data + 7);
  }
  if (info)
  {
    *info = net.info;
  }
  return status;
}

uint8_t netInfo(net_info_t *info)
{
  return netInfoCall(info, NET_TIMEOUT_MS, NET_ATTEMPTS);
}

const net_info_t *netInfoCached(void)
{
  return &net.info;
}

bool netIsConnected(void)
{
  return netIsPresent() && net.info.state == NET_STATE_CONNECTED;
}

uint8_t netOff(void)
{
  memset(&net.info, 0, sizeof(net.info));
  return netCall(NET_CMD_OFF, NULL, 0, NULL, 0, NULL, NET_TIMEOUT_MS);
}

uint8_t netWifiScan(void)
{
  return netCall(NET_CMD_WIFI_SCAN, NULL, 0, NULL, 0, NULL, NET_TIMEOUT_MS);
}

uint8_t netWifiList(net_ap_t *list, unsigned max, unsigned *count)
{
  static uint8_t data[NET_LINK_PAYLOAD_MAX];
  uint16_t       got    = 0;
  uint8_t        status = netCall(NET_CMD_WIFI_LIST, NULL, 0, data, sizeof(data), &got, NET_TIMEOUT_MS);
  *count                = 0;
  if (status != NET_ST_OK || !got)
  {
    return status;
  }
  unsigned pos = 1;
  for (unsigned i = 0; i < data[0] && pos + 3 <= got; i++)
  {
    unsigned length = data[pos + 2];
    if (pos + 3 + length > got)
    {
      break;
    }
    if (length && length < NET_SSID_SIZE && *count < max)
    {
      net_ap_t *ap = &list[(*count)++];
      ap->rssi     = (int8_t)data[pos];
      ap->secure   = data[pos + 1] != 0;
      memcpy(ap->ssid, data + pos + 3, length);
      ap->ssid[length] = 0;
    }
    pos += 3 + length;
  }
  for (unsigned i = 1; i < *count; i++) /* The ESP sorts too; keep the order safe here. */
  {
    net_ap_t ap = list[i];
    unsigned j  = i;
    for (; j && list[j - 1].rssi < ap.rssi; j--)
    {
      list[j] = list[j - 1];
    }
    list[j] = ap;
  }
  return status;
}

uint8_t netWifiJoin(const char *ssid, const char *password)
{
  uint8_t data[2 + NET_SSID_SIZE + NET_PASSWORD_SIZE];
  size_t  s = strlen(ssid), p = strlen(password);
  if (!s || s >= NET_SSID_SIZE || p >= NET_PASSWORD_SIZE)
  {
    return NET_ST_BAD_REQUEST;
  }
  data[0] = (uint8_t)s;
  memcpy(data + 1, ssid, s);
  data[1 + s] = (uint8_t)p;
  memcpy(data + 2 + s, password, p);
  return netCall(NET_CMD_WIFI_JOIN, data, (uint16_t)(2 + s + p), NULL, 0, NULL, NET_TIMEOUT_MS);
}

uint8_t netLteJoin(const char *apn)
{
  uint8_t data[1 + NET_APN_SIZE];
  size_t  n = strlen(apn);
  if (n >= NET_APN_SIZE)
  {
    return NET_ST_BAD_REQUEST;
  }
  data[0] = (uint8_t)n;
  memcpy(data + 1, apn, n);
  return netCall(NET_CMD_LTE_JOIN, data, (uint16_t)(1 + n), NULL, 0, NULL, NET_TIMEOUT_MS);
}

uint8_t netHttpGet(const char *url, uint16_t *code, uint32_t *length)
{
  uint8_t  request[2 + 256];
  uint8_t  data[6];
  uint16_t got = 0;
  size_t   n   = strlen(url);
  *code        = 0;
  *length      = UINT32_MAX;
  if (n > 255)
  {
    return NET_ST_BAD_REQUEST;
  }
  request[0] = (uint8_t)n;
  request[1] = 0;
  memcpy(request + 2, url, n);
  uint8_t status = netCall(NET_CMD_HTTP_GET, request, (uint16_t)(2 + n), data, sizeof(data), &got, NET_HTTP_GET_MS);
  if (status == NET_ST_OK && got >= 6)
  {
    *code   = (uint16_t)(data[0] | data[1] << 8);
    *length = (uint32_t)data[2] | (uint32_t)data[3] << 8 | (uint32_t)data[4] << 16 | (uint32_t)data[5] << 24;
  }
  return status;
}

uint8_t netHttpRead(void *data, uint16_t max, uint16_t *size)
{
  if (max > NET_HTTP_READ_MAX)
  {
    max = NET_HTTP_READ_MAX; /* Callers may pass any buffer size; the rest comes in the next read. */
  }
  uint8_t request[2] = {(uint8_t)max, (uint8_t)(max >> 8)};
  return netCall(NET_CMD_HTTP_READ, request, sizeof(request), data, max, size, NET_HTTP_READ_MS);
}

uint8_t netHttpClose(void)
{
  return netCall(NET_CMD_HTTP_CLOSE, NULL, 0, NULL, 0, NULL, NET_TIMEOUT_MS);
}

const char *netStatusText(uint8_t status)
{
  switch (status)
  {
    case NET_ST_OK:
      return "OK";
    case NET_ST_BUSY:
      return "Busy";
    case NET_ST_BAD_REQUEST:
      return "Bad request";
    case NET_ST_NOT_CONNECTED:
      return "Not connected";
    case NET_ST_HTTP:
      return "HTTP error";
    case NET_ST_TIMEOUT:
      return "Net timeout";
    case NET_ST_UNKNOWN_CMD:
      return "ESP too old";
    case NET_ST_NO_LINK:
      return "No ESP link";
    default:
      return "Net error";
  }
}

net_store_t *netSettings(void)
{
  return &net.settings;
}

void netSettingsReload(bool mounted)
{
  net.mounted = mounted;
  if (!mounted || !netStoreLoad(&net.settings))
  {
    memset(&net.settings, 0, sizeof(net.settings));
  }
  net.rejoin = net.settings.mode != NET_MODE_OFF;
}

bool netSettingsSave(void)
{
  return net.mounted && netStoreSave(&net.settings);
}

/* Rejoins the network saved on the card once the ESP answers, so Install works right after power-up. */
static void netRejoin(void)
{
  net.rejoin = false;
  /* Only a fresh (idle) ESP is joined automatically: after a failure the user retries from the menu,
   * so a missing SIM or a wrong password does not restart the radio over and over. */
  if (netInfo(NULL) != NET_ST_OK || net.info.state != NET_STATE_IDLE)
  {
    return;
  }
  const net_store_t *s = &net.settings;
  if (s->mode == NET_MODE_WIFI && s->ssid[0])
  {
    const char *password = netStorePassword(s, s->ssid);
    netWifiJoin(s->ssid, password ? password : "");
  }
  else if (s->mode == NET_MODE_LTE)
  {
    netLteJoin(s->apn[0] ? s->apn : net_apns[0].apn);
  }
}

void netUpdate(void)
{
  if ((uint32_t)(millis() - net.probe) < (netIsPresent() ? NET_STATUS_MS : NET_PROBE_MS))
  {
    return;
  }
  net.probe = millis();
  if (!netIsPresent())
  {
    /* Single short attempt: the ESP has its own power and may come and go. */
    uint8_t data[40];
    uint16_t got = 0;
    if (netLinkCall(NET_CMD_HELLO, NULL, 0, data, sizeof(data) - 1, &got, NET_PROBE_TIMEOUT, 1) == NET_ST_OK)
    {
      net.present = true;
      net.seen    = millis();
      net.rejoin  = net.settings.mode != NET_MODE_OFF;
      netHelloParse(data, got);
    }
    return;
  }
  if (net.rejoin)
  {
    netRejoin();
    return;
  }
  netInfoCall(NULL, NET_STATUS_TIMEOUT, 1); /* Keeps netInfoCached(), netIsConnected() and presence current. */
}

static void cliNet(cli_args_t *args)
{
  uint8_t st;
  if (args->argc == 1 && args->isStr(0, "hello"))
  {
    st = netHello();
    cliPrintf("HELLO    : %s %s\n", netStatusText(st), st == NET_ST_OK ? net.version : "");
    return;
  }
  if (args->argc == 1 && args->isStr(0, "status"))
  {
    net_info_t info;
    st = netInfo(&info);
    cliPrintf("STATUS   : %s, mode=%u state=%u rssi=%d ip=%u.%u.%u.%u %s\n", netStatusText(st), info.mode, info.state,
              info.rssi, info.ip[0], info.ip[1], info.ip[2], info.ip[3], info.detail);
    return;
  }
  if (args->argc == 1 && args->isStr(0, "scan"))
  {
    st = netWifiScan();
    for (unsigned i = 0; i < 50 && st == NET_ST_OK; i++)
    {
      static net_ap_t aps[NET_AP_MAX];
      unsigned        count = 0;
      delay(200);
      st = netWifiList(aps, NET_AP_MAX, &count);
      if (st == NET_ST_BUSY)
      {
        st = NET_ST_OK;
        continue;
      }
      for (unsigned n = 0; n < count; n++)
      {
        cliPrintf("%4d %c %s\n", aps[n].rssi, aps[n].secure ? '*' : ' ', aps[n].ssid);
      }
      break;
    }
    cliPrintf("SCAN     : %s\n", netStatusText(st));
    return;
  }
  if ((args->argc == 2 || args->argc == 3) && args->isStr(0, "wifi"))
  {
    st = netWifiJoin(args->getStr(1), args->argc == 3 ? args->getStr(2) : "");
    cliPrintf("WIFI     : %s (poll with net status)\n", netStatusText(st));
    return;
  }
  if (args->argc == 2 && args->isStr(0, "lte"))
  {
    st = netLteJoin(args->getStr(1));
    cliPrintf("LTE      : %s (poll with net status)\n", netStatusText(st));
    return;
  }
  if (args->argc == 1 && args->isStr(0, "off"))
  {
    cliPrintf("OFF      : %s\n", netStatusText(netOff()));
    return;
  }
  if (args->argc == 2 && args->isStr(0, "get"))
  {
    uint16_t code;
    uint32_t length, total = 0, crc = UINT32_MAX;
    st = netHttpGet(args->getStr(1), &code, &length);
    cliPrintf("GET      : %s, HTTP %u, length=%ld\n", netStatusText(st), code, (long)length);
    while (st == NET_ST_OK && code == 200)
    {
      static uint8_t chunk[1024];
      uint16_t       got = 0;
      st                 = netHttpRead(chunk, sizeof(chunk), &got);
      if (st != NET_ST_OK || !got)
      {
        break;
      }
      for (uint16_t i = 0; i < got; i++)
      {
        crc ^= chunk[i];
        for (unsigned bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
        }
      }
      total += got;
    }
    netHttpClose();
    cliPrintf("BODY     : %s, %lu bytes, CRC32=%08lX\n", netStatusText(st), (unsigned long)total,
              (unsigned long)~crc);
    return;
  }
  cliPrintf("net hello | status | scan | wifi <ssid> [pw] | lte <apn> | off | get <https-url>\n"
            "ESP link: USART6 PC6=TX->ESP GPIO48, PC7=RX<-ESP GPIO45, %lu bps; %s\n",
            (unsigned long)NET_LINK_BAUD, netIsPresent() ? net.version : "ESP not found");
}

static bool netInit(void)
{
  uint32_t uid[3] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
  netStoreSetKey((const uint8_t *)uid);
  netLinkBind(&net_io);
  netLinkSeed((uint8_t)(SysTick->VAL ^ (SysTick->VAL >> 8) ^ uid[0])); /* Boot timing varies; good enough to avoid the ESP's cached reply. */
  bool ok = uartOpen(HW_UART_CH_NET, NET_LINK_BAUD);
  netSettingsReload(sdIsDetected() && fatfsIsMount());
  return cliAdd("net", cliNet) && ok;
}

MODULE_DEF(net)
{
  .name     = "net",
  .priority = MODULE_PRI_NORMAL,
  .init     = netInit,
};
