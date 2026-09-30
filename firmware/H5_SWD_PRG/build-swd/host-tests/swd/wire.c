
#include "swd_transport.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool output, clk, data_pin;
static unsigned rises, nr, at;
static unsigned bits[64];
void swdPortActivity(void) {}
static bool edge_output[256], edge_data[256];
void swdClockWrite(bool high) {
  if (high && !clk) {
    assert(rises < 256);
    edge_output[rises] = output; edge_data[rises++] = data_pin;
  }
  clk = high;
}
void swdDataOutput(bool enable) { output = enable; }
void swdDataWrite(bool high) { data_pin = high; }
bool swdDataRead(void) {
  assert(!output && !clk); /* Buffered input is only sampled while released. */
  assert(at < nr); return bits[at++];
}
void swdPortDelay(void) {}
static void reset(void) {
  output = true; clk = true; data_pin = true; rises = nr = at = 0;
  memset(edge_output, 0, sizeof(edge_output));
}
static void queue(unsigned value, unsigned count) {
  for (unsigned i = 0; i < count; i++) bits[nr++] = (value >> i) & 1;
}
static unsigned decode(unsigned start, unsigned count) {
  unsigned value = 0;
  for (unsigned i = 0; i < count; i++) value |= (unsigned)edge_data[start+i] << i;
  return value;
}
int main(void) {
  reset(); swdWireConnect();
  assert(rises == 136);
  for (unsigned i = 0; i < 56; i++) assert(edge_data[i] && edge_output[i]);
  assert(decode(56, 16) == 0xE79E);
  for (unsigned i = 72; i < 128; i++) assert(edge_data[i]);
  assert(decode(128, 8) == 0);

  reset(); queue(1, 3); queue(0x2BA01477, 32); queue(0, 1);
  unsigned value = 0;
  assert(swdWireTransfer(false, true, 0, &value) == SWD_ACK_OK);
  assert(value == 0x2BA01477 && at == nr && rises == 54);
  assert(decode(0, 8) == 0xA5); /* Known DPIDR request packet. */
  for (unsigned i = 0; i < 8; i++) assert(edge_output[i]);
  for (unsigned i = 8; i < 46; i++) assert(!edge_output[i]);
  for (unsigned i = 46; i < 54; i++) assert(edge_output[i] && !edge_data[i]);

  reset(); queue(1, 3); queue(0x2BA01477, 32); queue(1, 1);
  value = 0xDEADBEEF;
  assert(swdWireTransfer(false, true, 0, &value) == SWD_ACK_PARITY);
  assert(value == 0xDEADBEEF); /* Never publish corrupt data. */

  reset(); queue(1, 3); value = 0x80000001;
  assert(swdWireTransfer(true, false, 0xC, &value) == SWD_ACK_OK);
  assert(decode(0, 8) == 0xBB); /* AP DRW write request. */
  for (unsigned i = 8; i < 13; i++) assert(!edge_output[i]);
  for (unsigned i = 13; i < 46; i++) assert(edge_output[i]);
  assert(decode(13, 32) == value && !edge_data[45]);

  const unsigned acks[] = {2, 4, 7, 0};
  for (unsigned j = 0; j < 4; j++) {
    reset(); queue(acks[j], 3); value = 0x1234;
    assert(swdWireTransfer(false, true, 0, &value) == acks[j]);
    assert(value == 0x1234 && at == 3 && output);
    assert(rises == (j < 2 ? 21U : 54U));
  }
  puts("SWD wire: request bits, turnaround, parity, WAIT/FAULT/no-response passed");
}
