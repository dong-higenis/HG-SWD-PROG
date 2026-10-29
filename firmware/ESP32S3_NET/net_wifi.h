#ifndef NET_WIFI_H_
#define NET_WIFI_H_

#include <Arduino.h>

#include "net.h"

enum ApScanState : uint8_t
{
  APSCAN_NONE = 0,  // no result yet (never scanned / after OFF)
  APSCAN_BUSY,      // requested or running
  APSCAN_DONE,
  APSCAN_ERROR,
};

struct WifiApEntry
{
  int8_t  rssi;
  uint8_t auth;  // 0 open, 1 secured
  uint8_t ssid_len;
  char    ssid[WIFI_SSID_MAX + 1];
};

void    wifiInit(void);
void    wifiTask(void);  // advances scan / join state machines (loop context)
void    wifiOff(void);

uint8_t wifiScanStart(void);  // ST_OK or ST_BUSY (join in progress)
uint8_t wifiScanState(void);
const WifiApEntry *wifiScanResults(uint8_t *count);

void    wifiJoin(const char *ssid, const char *pw);
bool    wifiIsConnected(void);
void    wifiGetStatus(NetStatusInfo *st);

#endif
