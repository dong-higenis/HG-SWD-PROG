#ifndef NET_LTE_H_
#define NET_LTE_H_

#include <Arduino.h>

#include "net.h"

// LTE over PPP (core PPP library + esp_modem). The blocking modem calls run in a
// dedicated FreeRTOS task so the H5 link in loop() keeps answering STATUS polls.
// All functions here are non-blocking and safe to call from loop().

void lteInit(void);
void lteStart(const char *apn);  // (re)starts: power cycle, attach, dial
void lteStop(void);              // modem power is cut immediately, PPP torn down by the task
bool lteIsActiveWithApn(const char *apn);  // connecting/connected with this APN
bool lteIsConnected(void);
void lteGetStatus(NetStatusInfo *st);

#endif
