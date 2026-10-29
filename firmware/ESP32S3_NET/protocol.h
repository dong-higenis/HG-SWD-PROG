#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

// Executes one request and writes the response payload (status byte first) to `out`.
// Returns the payload length (>= 1).
size_t protocolHandle(uint8_t cmd, const uint8_t *req, uint16_t req_len, uint8_t *out, size_t out_cap);

#endif
