#ifndef NET_H_
#define NET_H_

#include <Arduino.h>

#include "protocol_defs.h"

// Network coordinator: keeps Wi-Fi and LTE mutually exclusive and builds STATUS.

struct NetStatusInfo
{
  uint8_t mode;   // NetMode
  uint8_t state;  // NetState
  int8_t  rssi;   // dBm, 0 = unknown
  uint8_t ip[4];  // a.b.c.d, 0.0.0.0 when not connected
  char    detail[STATUS_DETAIL_MAX + 1];
};

void    netInit(void);
void    netTask(void);  // call from loop()

uint8_t netOff(void);
uint8_t netWifiScan(void);
uint8_t netWifiJoin(const char *ssid, const char *pw);
uint8_t netLteJoin(const char *apn);

void    netGetStatus(NetStatusInfo *st);
bool    netIsConnected(void);

const char *netModeName(uint8_t mode);
const char *netStateName(uint8_t state);

#endif
