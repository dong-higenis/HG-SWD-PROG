#include "net_http.h"

#include <Network.h>
#include <NetworkClientSecure.h>
#include <esp_crt_bundle.h>

#include "ca_bundle.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "protocol_defs.h"

// A hand-written HTTP/1.1 client on top of NetworkClientSecure: HTTPClient's
// stream hands back raw chunked framing, and we need body bytes only, in
// caller-sized pieces, without String allocations.

namespace
{
enum class BodyMode : uint8_t
{
  NONE,         // 1xx/204/304 or zero length
  LENGTH,       // Content-Length
  CHUNKED,      // Transfer-Encoding: chunked
  UNTIL_CLOSE,  // neither: body ends when the server closes
};

enum class ChunkPhase : uint8_t
{
  SIZE,      // expecting "<hex>[;ext]"
  DATA,      // `remaining` bytes of chunk data
  DATA_END,  // CRLF after chunk data
  TRAILER,   // trailer lines until an empty line
};

constexpr size_t RX_BUF_SIZE = 1024;
constexpr size_t HTTP_LINE_MAX    = 512;  // longer header lines are truncated (only a few headers matter)
constexpr size_t HOST_MAX    = 253;

NetworkClientSecure client;
bool       tls_open     = false;
bool       session_open = false;  // between a successful HTTP_GET and HTTP_CLOSE
bool       body_done    = true;
bool       body_broken  = false;
const char *broken_reason = "";
BodyMode   body_mode    = BodyMode::NONE;
ChunkPhase chunk_phase  = ChunkPhase::SIZE;
uint32_t   remaining    = 0;  // LENGTH: body bytes left; CHUNKED: bytes left in this chunk
uint32_t   body_total   = 0;

uint8_t rx_buf[RX_BUF_SIZE];
size_t  rx_pos = 0;
size_t  rx_len = 0;

char   line[HTTP_LINE_MAX];
size_t line_len   = 0;
bool   line_ready = false;

char     url_host[HOST_MAX + 1];
char     url_path[HTTP_URL_MAX + 2];
uint16_t url_port = 443;
char     request[HTTP_URL_MAX + HOST_MAX + 192];

void setReason(char *reason, size_t size, const char *text)
{
  if (reason != nullptr && size > 0)
  {
    strlcpy(reason, text, size);
  }
}

bool expired(uint32_t deadline)
{
  return (int32_t)(millis() - deadline) >= 0;
}

uint32_t msLeft(uint32_t deadline)
{
  return expired(deadline) ? 0 : deadline - millis();
}

bool parseUrl(const char *url)
{
  if (strncasecmp(url, "https://", 8) != 0)
  {
    return false;
  }
  for (const char *c = url; *c; c++)
  {
    if ((uint8_t)*c <= 0x20 || (uint8_t)*c >= 0x7F)
    {
      return false;
    }
  }

  const char *p        = url + 8;
  const char *host_end = p;
  while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#' && *host_end != ':')
  {
    host_end++;
  }
  size_t host_len = (size_t)(host_end - p);
  if (host_len == 0 || host_len > HOST_MAX || memchr(p, '@', host_len) || memchr(p, '[', host_len))
  {
    return false;  // no userinfo, no IPv6 literals
  }
  memcpy(url_host, p, host_len);
  url_host[host_len] = '\0';
  p                  = host_end;

  url_port = 443;
  if (*p == ':')
  {
    p++;
    uint32_t port   = 0;
    int      digits = 0;
    while (*p >= '0' && *p <= '9')
    {
      port = port * 10 + (uint32_t)(*p++ - '0');
      if (++digits > 5)
      {
        return false;
      }
    }
    if (digits == 0 || port == 0 || port > 65535)
    {
      return false;
    }
    url_port = (uint16_t)port;
  }

  const char *frag = strchr(p, '#');
  size_t      len  = frag ? (size_t)(frag - p) : strlen(p);
  if (len == 0)
  {
    strcpy(url_path, "/");
  }
  else if (*p == '/')
  {
    memcpy(url_path, p, len);
    url_path[len] = '\0';
  }
  else if (*p == '?')
  {
    url_path[0] = '/';
    memcpy(url_path + 1, p, len);
    url_path[len + 1] = '\0';
  }
  else
  {
    return false;
  }
  return true;
}

void stopTls(void)
{
  if (tls_open)
  {
    client.stop();
    tls_open = false;
  }
  rx_pos = rx_len = 0;
}

// Refills rx_buf when it is empty. >0: bytes buffered, 0: nothing yet, -1: connection closed.
int fill(void)
{
  if (rx_pos < rx_len)
  {
    return (int)(rx_len - rx_pos);
  }
  rx_pos = rx_len = 0;
  int avail       = client.available();
  if (avail > 0)
  {
    int r = client.read(rx_buf, (size_t)avail < RX_BUF_SIZE ? (size_t)avail : RX_BUF_SIZE);
    if (r > 0)
    {
      rx_len = (size_t)r;
      return r;
    }
  }
  return client.connected() ? 0 : -1;
}

// Incremental line reader. 1: line complete (CR/LF stripped), 0: need more data, -1: closed.
int readLine(void)
{
  if (line_ready)
  {
    line_len   = 0;
    line_ready = false;
  }
  for (;;)
  {
    int n = fill();
    if (n <= 0)
    {
      return n;
    }
    while (rx_pos < rx_len)
    {
      char c = (char)rx_buf[rx_pos++];
      if (c == '\n')
      {
        if (line_len > 0 && line[line_len - 1] == '\r')
        {
          line_len--;
        }
        line[line_len] = '\0';
        line_ready     = true;
        return 1;
      }
      if (line_len < HTTP_LINE_MAX - 1)
      {
        line[line_len++] = c;
      }
    }
  }
}

// 1: line, 0: deadline passed, -1: closed
int waitLine(uint32_t deadline)
{
  for (;;)
  {
    int r = readLine();
    if (r != 0)
    {
      return r;
    }
    if (expired(deadline))
    {
      return 0;
    }
    delay(1);
  }
}

// Body bytes: drains rx_buf first, then reads straight into dst.
int takeBody(uint8_t *dst, size_t want)
{
  if (rx_pos < rx_len)
  {
    size_t n = rx_len - rx_pos;
    if (n > want)
    {
      n = want;
    }
    memcpy(dst, &rx_buf[rx_pos], n);
    rx_pos += n;
    return (int)n;
  }
  int avail = client.available();
  if (avail > 0)
  {
    int r = client.read(dst, (size_t)avail < want ? (size_t)avail : want);
    if (r > 0)
    {
      return r;
    }
  }
  return client.connected() ? 0 : -1;
}

// "Name: value" -> value (leading blanks skipped) if the name matches, else nullptr.
const char *headerValue(const char *hdr, const char *name)
{
  size_t n = strlen(name);
  if (strncasecmp(hdr, name, n) != 0 || hdr[n] != ':')
  {
    return nullptr;
  }
  const char *v = hdr + n + 1;
  while (*v == ' ' || *v == '\t')
  {
    v++;
  }
  return v;
}

bool containsNoCase(const char *s, const char *word)
{
  size_t n = strlen(word);
  for (; *s; s++)
  {
    if (strncasecmp(s, word, n) == 0)
    {
      return true;
    }
  }
  return false;
}

bool parseDecimal(const char *s, uint32_t *out)
{
  uint64_t v      = 0;
  int      digits = 0;
  while (*s >= '0' && *s <= '9')
  {
    v = v * 10 + (uint64_t)(*s++ - '0');
    if (v >= HTTP_LENGTH_UNKNOWN)
    {
      return false;
    }
    digits++;
  }
  while (*s == ' ' || *s == '\t')
  {
    s++;
  }
  if (digits == 0 || *s != '\0')
  {
    return false;
  }
  *out = (uint32_t)v;
  return true;
}

bool parseStatusLine(const char *s, uint16_t *code)
{
  if (strncmp(s, "HTTP/", 5) != 0)
  {
    return false;
  }
  const char *sp = strchr(s, ' ');
  if (sp == nullptr)
  {
    return false;
  }
  sp++;
  if (!(sp[0] >= '1' && sp[0] <= '5' && sp[1] >= '0' && sp[1] <= '9' && sp[2] >= '0' && sp[2] <= '9') ||
      (sp[3] != '\0' && sp[3] != ' '))
  {
    return false;
  }
  *code = (uint16_t)((sp[0] - '0') * 100 + (sp[1] - '0') * 10 + (sp[2] - '0'));
  return true;
}

void finishBody(void)
{
  body_done = true;
  stopTls();  // free the TLS session now; the session stays "open" until HTTP_CLOSE
  LOGV("HTTP", "body complete, %lu bytes", (unsigned long)body_total);
}

void breakBody(const char *reason)
{
  body_broken   = true;
  broken_reason = reason;
  stopTls();
  LOGI("HTTP", "body error: %s after %lu bytes", reason, (unsigned long)body_total);
}

// Handles one complete framing line of a chunked body. false = malformed.
bool chunkLine(void)
{
  switch (chunk_phase)
  {
    case ChunkPhase::SIZE:
    {
      uint32_t    size   = 0;
      int         digits = 0;
      const char *p      = line;
      for (;; p++)
      {
        char    c = *p;
        uint8_t d;
        if (c >= '0' && c <= '9')
        {
          d = (uint8_t)(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
          d = (uint8_t)(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
          d = (uint8_t)(c - 'A' + 10);
        }
        else
        {
          break;
        }
        if (++digits > 8)
        {
          return false;
        }
        size = (size << 4) | d;
      }
      if (digits == 0 || (*p != '\0' && *p != ';' && *p != ' ' && *p != '\t'))
      {
        return false;
      }
      if (size == 0)
      {
        chunk_phase = ChunkPhase::TRAILER;
      }
      else
      {
        remaining   = size;
        chunk_phase = ChunkPhase::DATA;
      }
      return true;
    }

    case ChunkPhase::DATA_END:
      if (line[0] != '\0')
      {
        return false;
      }
      chunk_phase = ChunkPhase::SIZE;
      return true;

    case ChunkPhase::TRAILER:
      if (line[0] == '\0')
      {
        finishBody();
      }
      return true;

    case ChunkPhase::DATA:
      break;
  }
  return false;
}

uint8_t failGet(uint8_t status, const char *text, char *reason, size_t reason_size)
{
  setReason(reason, reason_size, text);
  stopTls();
  LOGI("HTTP", "GET https://%s%s failed: %s", url_host, url_path, text);
  return status;
}
}  // namespace

void httpInit(void)
{
  // Same check the TLS layer does; if it fails the core silently uses its built-in bundle.
  esp_err_t err = esp_crt_bundle_set(ca_bundle_bin, sizeof(ca_bundle_bin));
  if (err == ESP_OK)
  {
    LOGI("HTTP", "CA bundle ok (%u roots)", (unsigned)CA_BUNDLE_CERT_COUNT);
  }
  else
  {
    LOGI("HTTP", "CA bundle REJECTED (%s) - regenerate ca_bundle.h", esp_err_to_name(err));
  }
}

uint8_t httpGet(const char *url, uint16_t *code, uint32_t *length, char *reason, size_t reason_size)
{
  httpClose();
  *code   = 0;
  *length = 0;
  url_host[0] = '\0';
  url_path[0] = '\0';

  if (!parseUrl(url))
  {
    setReason(reason, reason_size, "bad url");
    return ST_BAD_REQUEST;
  }
  if (!netIsConnected())
  {
    return ST_NOT_CONNECTED;
  }

  uint32_t t0       = millis();
  uint32_t deadline = t0 + HTTP_GET_TIMEOUT_MS;

  IPAddress addr;
  if (!Network.hostByName(url_host, addr))
  {
    return failGet(ST_HTTP, "DNS failed", reason, reason_size);
  }

  uint32_t left = msLeft(deadline);
  if (left == 0)
  {
    return failGet(ST_TIMEOUT, "DNS timeout", reason, reason_size);
  }

  client.setCACertBundle(ca_bundle_bin, sizeof(ca_bundle_bin));
  client.setHandshakeTimeout(left / 1000 ? left / 1000 : 1);
  tls_open = true;  // stop() must run even if connect() fails half way
  if (!client.connect(url_host, url_port, (int32_t)left))
  {
    char err_text[48] = "";
    int  err          = client.lastError(err_text, sizeof(err_text));
    char msg[64];
    if (err == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
    {
      strlcpy(msg, "TLS cert verify failed", sizeof(msg));
    }
    else if (err == 0 || err == -1)
    {
      strlcpy(msg, expired(deadline) ? "connect timeout" : "connect failed", sizeof(msg));
    }
    else
    {
      snprintf(msg, sizeof(msg), "TLS -0x%04X %s", (unsigned)-err, err_text);
    }
    return failGet(expired(deadline) ? ST_TIMEOUT : ST_HTTP, msg, reason, reason_size);
  }

  char port_suffix[8] = "";
  if (url_port != 443)
  {
    snprintf(port_suffix, sizeof(port_suffix), ":%u", url_port);
  }
  int n = snprintf(request, sizeof(request),
                   "GET %s HTTP/1.1\r\n"
                   "Host: %s%s\r\n"
                   "User-Agent: " NET_FW_NAME "/" NET_FW_VERSION "\r\n"
                   "Accept: */*\r\n"
                   "Accept-Encoding: identity\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   url_path, url_host, port_suffix);
  if (n <= 0 || (size_t)n >= sizeof(request))
  {
    return failGet(ST_BAD_REQUEST, "url too long", reason, reason_size);
  }
  if (client.write((const uint8_t *)request, (size_t)n) != (size_t)n)
  {
    return failGet(ST_HTTP, "send failed", reason, reason_size);
  }

  // Status line + headers. Interim 1xx responses (except 101) are skipped.
  rx_pos = rx_len = 0;
  line_len        = 0;
  line_ready      = false;

  uint16_t status     = 0;
  bool     chunked    = false;
  bool     have_len   = false;
  bool     bad_len    = false;
  uint32_t content_len = 0;

  for (;;)
  {
    int r = waitLine(deadline);
    if (r == 0)
    {
      return failGet(ST_TIMEOUT, "header timeout", reason, reason_size);
    }
    if (r < 0)
    {
      return failGet(ST_HTTP, "closed before response", reason, reason_size);
    }
    if (!parseStatusLine(line, &status))
    {
      return failGet(ST_HTTP, "bad status line", reason, reason_size);
    }

    chunked  = false;
    have_len = false;
    bad_len  = false;
    for (;;)
    {
      r = waitLine(deadline);
      if (r == 0)
      {
        return failGet(ST_TIMEOUT, "header timeout", reason, reason_size);
      }
      if (r < 0)
      {
        return failGet(ST_HTTP, "closed in headers", reason, reason_size);
      }
      if (line[0] == '\0')
      {
        break;
      }
      const char *v;
      if ((v = headerValue(line, "Content-Length")) != nullptr)
      {
        uint32_t len;
        if (parseDecimal(v, &len) && (!have_len || len == content_len))
        {
          content_len = len;
          have_len    = true;
        }
        else
        {
          bad_len = true;
        }
      }
      else if ((v = headerValue(line, "Transfer-Encoding")) != nullptr)
      {
        if (containsNoCase(v, "chunked"))
        {
          chunked = true;
        }
      }
    }

    if (status >= 200 || status == 101)
    {
      break;
    }
  }

  body_total  = 0;
  body_broken = false;
  if (status < 200 || status == 204 || status == 304)
  {
    body_mode = BodyMode::NONE;
    *length   = 0;
  }
  else if (chunked)  // takes precedence over Content-Length (RFC 9112 6.3)
  {
    body_mode   = BodyMode::CHUNKED;
    chunk_phase = ChunkPhase::SIZE;
    *length     = HTTP_LENGTH_UNKNOWN;
  }
  else if (bad_len)
  {
    return failGet(ST_HTTP, "bad Content-Length", reason, reason_size);
  }
  else if (have_len)
  {
    body_mode = BodyMode::LENGTH;
    remaining = content_len;
    *length   = content_len;
  }
  else
  {
    body_mode = BodyMode::UNTIL_CLOSE;
    *length   = HTTP_LENGTH_UNKNOWN;
  }

  session_open = true;
  body_done    = false;
  if (body_mode == BodyMode::NONE || (body_mode == BodyMode::LENGTH && remaining == 0))
  {
    finishBody();
  }

  *code = status;
  LOGI("HTTP", "GET https://%s%s -> %u len=%ld (%lums)", url_host, url_path, status,
       *length == HTTP_LENGTH_UNKNOWN ? -1L : (long)*length, (unsigned long)(millis() - t0));
  return ST_OK;
}

uint8_t httpRead(uint8_t *buf, uint16_t max, uint16_t *out_len, char *reason, size_t reason_size)
{
  *out_len = 0;
  if (!session_open)
  {
    setReason(reason, reason_size, "no request");
    return ST_ERROR;
  }
  if (body_broken)
  {
    setReason(reason, reason_size, broken_reason);
    return ST_HTTP;
  }
  if (body_done)
  {
    return ST_OK;  // 0 bytes = end of body
  }

  uint32_t deadline = millis() + HTTP_READ_TIMEOUT_MS;
  uint16_t got      = 0;

  while (got < max && !body_done && !body_broken)
  {
    int r;
    if (body_mode == BodyMode::CHUNKED && chunk_phase != ChunkPhase::DATA)
    {
      r = readLine();
      if (r == 1)
      {
        if (!chunkLine())
        {
          breakBody("bad chunk");
        }
        continue;
      }
    }
    else
    {
      size_t want = (size_t)(max - got);
      if (body_mode != BodyMode::UNTIL_CLOSE && want > remaining)
      {
        want = remaining;
      }
      r = takeBody(buf + got, want);
      if (r > 0)
      {
        got += (uint16_t)r;
        body_total += (uint32_t)r;
        if (body_mode != BodyMode::UNTIL_CLOSE)
        {
          remaining -= (uint32_t)r;
          if (remaining == 0)
          {
            if (body_mode == BodyMode::LENGTH)
            {
              finishBody();
            }
            else
            {
              chunk_phase = ChunkPhase::DATA_END;
            }
          }
        }
        continue;
      }
    }

    if (r < 0)  // connection closed
    {
      if (body_mode == BodyMode::UNTIL_CLOSE)
      {
        finishBody();
      }
      else
      {
        breakBody("closed early");
      }
      break;
    }
    if (got > 0)
    {
      break;  // return what we have instead of waiting for a full buffer
    }
    if (expired(deadline))
    {
      setReason(reason, reason_size, "no data");
      return ST_TIMEOUT;
    }
    delay(1);
  }

  *out_len = got;
  if (got == 0 && body_broken)
  {
    setReason(reason, reason_size, broken_reason);
    return ST_HTTP;
  }
  return ST_OK;
}

void httpClose(void)
{
  if (session_open)
  {
    LOGV("HTTP", "close");
  }
  stopTls();
  session_open = false;
  body_done    = true;
  body_broken  = false;
}

bool httpIsOpen(void)
{
  return session_open;
}
