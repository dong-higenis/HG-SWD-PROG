#ifndef LINK_H_
#define LINK_H_

#include <Arduino.h>

// Framed UART link to the H5 master (Serial2). See PROTOCOL.md.

struct LinkStats
{
  uint32_t frames;      // valid requests handled (incl. duplicates)
  uint32_t duplicates;  // answered from the retransmit cache
  uint32_t crc_errors;
  uint32_t timeouts;    // partial frames dropped by the inter-byte timeout
  uint32_t oversize;    // len > FRAME_MAX_PAYLOAD
};

void linkInit(void);
void linkPoll(void);  // returns after the RX buffer is drained; may block inside a handler
bool linkActive(void);  // true while bytes were processed in the last poll
void linkGetStats(LinkStats *stats);

#endif
