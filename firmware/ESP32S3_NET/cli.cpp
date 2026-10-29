#include "cli.h"

#include <Arduino.h>

#include "config.h"
#include "link.h"
#include "log.h"
#include "net.h"
#include "net_http.h"
#include "net_wifi.h"
#include "protocol_defs.h"

namespace
{
constexpr int    CLI_MAX_ARGS   = 4;
constexpr size_t GET_DUMP_BYTES = 64;

char   line[CLI_LINE_MAX];
size_t line_len  = 0;
bool   scan_wait = false;

const char *statusName(uint8_t st)
{
  switch (st)
  {
    case ST_OK:            return "OK";
    case ST_BUSY:          return "BUSY";
    case ST_ERROR:         return "ERROR";
    case ST_BAD_REQUEST:   return "BAD_REQUEST";
    case ST_NOT_CONNECTED: return "NOT_CONNECTED";
    case ST_HTTP:          return "HTTP";
    case ST_TIMEOUT:       return "TIMEOUT";
    case ST_UNKNOWN_CMD:   return "UNKNOWN_CMD";
  }
  return "?";
}

// Splits on blanks; "double quotes" group words (for SSIDs with spaces).
int tokenize(char *s, char **argv)
{
  int argc = 0;
  while (*s && argc < CLI_MAX_ARGS)
  {
    while (*s == ' ' || *s == '\t')
    {
      s++;
    }
    if (*s == '\0')
    {
      break;
    }
    if (*s == '"')
    {
      argv[argc++] = ++s;
      while (*s && *s != '"')
      {
        s++;
      }
    }
    else
    {
      argv[argc++] = s;
      while (*s && *s != ' ' && *s != '\t')
      {
        s++;
      }
    }
    if (*s)
    {
      *s++ = '\0';
    }
  }
  return argc;
}

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len)
{
  crc = ~crc;
  while (len--)
  {
    crc ^= *data++;
    for (int k = 0; k < 8; k++)
    {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1)));
    }
  }
  return ~crc;
}

void printHelp(void)
{
  Serial.println(
    "commands:\n"
    "  status               network + link state\n"
    "  scan                 Wi-Fi scan (list printed when done)\n"
    "  wifi <ssid> [pw]     join Wi-Fi (quote an SSID with spaces)\n"
    "  lte <apn>            start LTE (\"\" = network default APN)\n"
    "  off                  Wi-Fi off, LTE modem power off\n"
    "  get <https-url>      HTTPS GET: code, length, CRC32, first bytes\n"
    "  log on|off           verbose link/HTTP trace");
}

void printStatus(void)
{
  NetStatusInfo st;
  LinkStats     ls;
  netGetStatus(&st);
  linkGetStats(&ls);
  Serial.printf("fw %s %s, proto %u, uptime %lus, heap %lu\n", NET_FW_NAME, NET_FW_VERSION, PROTO_VERSION,
                (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap());
  Serial.printf("mode %s, state %s, rssi %d, ip %u.%u.%u.%u, detail '%s'\n", netModeName(st.mode),
                netStateName(st.state), st.rssi, st.ip[0], st.ip[1], st.ip[2], st.ip[3], st.detail);
  Serial.printf("link: frames %lu, dup %lu, crc err %lu, timeouts %lu, oversize %lu; http %s\n",
                (unsigned long)ls.frames, (unsigned long)ls.duplicates, (unsigned long)ls.crc_errors,
                (unsigned long)ls.timeouts, (unsigned long)ls.oversize, httpIsOpen() ? "open" : "closed");
}

void printScan(void)
{
  uint8_t state = wifiScanState();
  if (state == APSCAN_ERROR)
  {
    Serial.println("scan failed");
    return;
  }
  uint8_t            count = 0;
  const WifiApEntry *aps   = wifiScanResults(&count);
  Serial.printf("%u networks:\n", count);
  for (uint8_t i = 0; i < count; i++)
  {
    Serial.printf("  %4d dBm  %s  %s\n", aps[i].rssi, aps[i].auth ? "lock" : "open", aps[i].ssid);
  }
}

void cmdGet(const char *url)
{
  uint16_t code   = 0;
  uint32_t length = 0;
  char     reason[64] = "";
  uint32_t t0     = millis();

  uint8_t st = httpGet(url, &code, &length, reason, sizeof(reason));
  if (st != ST_OK)
  {
    Serial.printf("GET failed: %s %s\n", statusName(st), reason);
    return;
  }
  Serial.printf("code %u, length %ld\n", code, length == HTTP_LENGTH_UNKNOWN ? -1L : (long)length);

  static uint8_t buf[HTTP_READ_MAX];
  uint8_t        head[GET_DUMP_BYTES];
  size_t         head_len = 0;
  uint32_t       total    = 0;
  uint32_t       crc      = 0;

  for (;;)
  {
    uint16_t n = 0;
    st         = httpRead(buf, sizeof(buf), &n, reason, sizeof(reason));
    if (st != ST_OK)
    {
      Serial.printf("READ failed after %lu bytes: %s %s\n", (unsigned long)total, statusName(st), reason);
      break;
    }
    if (n == 0)
    {
      break;
    }
    if (head_len < GET_DUMP_BYTES)
    {
      size_t take = min((size_t)n, GET_DUMP_BYTES - head_len);
      memcpy(&head[head_len], buf, take);
      head_len += take;
    }
    crc = crc32Update(crc, buf, n);
    total += n;
  }
  httpClose();

  uint32_t ms = millis() - t0;
  Serial.printf("body %lu bytes, crc32 %08lX, %lu ms (%lu B/s)\n", (unsigned long)total, (unsigned long)crc,
                (unsigned long)ms, (unsigned long)(ms ? (uint64_t)total * 1000 / ms : 0));
  for (size_t i = 0; i < head_len; i += 16)
  {
    Serial.printf("  %04u: ", (unsigned)i);
    for (size_t j = i; j < i + 16; j++)
    {
      if (j < head_len)
      {
        Serial.printf("%02X ", head[j]);
      }
      else
      {
        Serial.print("   ");
      }
    }
    Serial.print(" ");
    for (size_t j = i; j < i + 16 && j < head_len; j++)
    {
      Serial.print((head[j] >= 0x20 && head[j] < 0x7F) ? (char)head[j] : '.');
    }
    Serial.println();
  }
}

void execute(char *cmdline)
{
  char *argv[CLI_MAX_ARGS] = {};
  int   argc               = tokenize(cmdline, argv);
  if (argc == 0)
  {
    return;
  }
  const char *cmd = argv[0];

  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0)
  {
    printHelp();
  }
  else if (strcmp(cmd, "status") == 0)
  {
    printStatus();
  }
  else if (strcmp(cmd, "scan") == 0)
  {
    uint8_t st = netWifiScan();
    Serial.printf("scan: %s\n", statusName(st));
    scan_wait = st == ST_OK;
  }
  else if (strcmp(cmd, "wifi") == 0 && argc >= 2)
  {
    const char *pw = argc >= 3 ? argv[2] : "";
    if (strlen(argv[1]) > WIFI_SSID_MAX || strlen(pw) > WIFI_PW_MAX)
    {
      Serial.println("ssid/pw too long");
      return;
    }
    Serial.printf("wifi: %s\n", statusName(netWifiJoin(argv[1], pw)));
  }
  else if (strcmp(cmd, "lte") == 0)
  {
    const char *apn = argc >= 2 ? argv[1] : "";
    if (strlen(apn) > LTE_APN_MAX || strchr(apn, '"') != nullptr)
    {
      Serial.println("bad apn");
      return;
    }
    Serial.printf("lte: %s\n", statusName(netLteJoin(apn)));
  }
  else if (strcmp(cmd, "off") == 0)
  {
    Serial.printf("off: %s\n", statusName(netOff()));
  }
  else if (strcmp(cmd, "get") == 0 && argc >= 2)
  {
    cmdGet(argv[1]);
  }
  else if (strcmp(cmd, "log") == 0 && argc >= 2)
  {
    g_log_verbose = strcmp(argv[1], "on") == 0;
    Serial.printf("verbose log %s\n", g_log_verbose ? "on" : "off");
  }
  else
  {
    Serial.printf("unknown or incomplete command '%s' (try help)\n", cmd);
  }
}
}  // namespace

void cliInit(void)
{
  line_len = 0;
}

void cliTask(void)
{
  while (Serial.available() > 0)
  {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n')
    {
      if (line_len > 0)
      {
        Serial.println();
        line[line_len] = '\0';
        line_len       = 0;
        execute(line);
        Serial.print("> ");
      }
    }
    else if (c == '\b' || c == 0x7F)
    {
      if (line_len > 0)
      {
        line_len--;
        Serial.print("\b \b");
      }
    }
    else if (line_len < CLI_LINE_MAX - 1 && c >= 0x20)
    {
      line[line_len++] = c;
      Serial.print(c);
    }
  }

  if (scan_wait && wifiScanState() != APSCAN_BUSY)
  {
    scan_wait = false;
    printScan();
  }
}
