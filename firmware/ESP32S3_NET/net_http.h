#ifndef NET_HTTP_H_
#define NET_HTTP_H_

#include <Arduino.h>

// One HTTPS GET at a time, body streamed in pieces (HTTP_GET / HTTP_READ / HTTP_CLOSE).
// Return values are protocol status codes (protocol_defs.h); on ST_HTTP / ST_ERROR
// a short ASCII reason is written to `reason`.

constexpr uint32_t HTTP_LENGTH_UNKNOWN = 0xFFFFFFFFUL;

void    httpInit(void);
uint8_t httpGet(const char *url, uint16_t *code, uint32_t *length, char *reason, size_t reason_size);
uint8_t httpRead(uint8_t *buf, uint16_t max, uint16_t *out_len, char *reason, size_t reason_size);
void    httpClose(void);
bool    httpIsOpen(void);

#endif
