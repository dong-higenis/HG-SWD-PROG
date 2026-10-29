#include "net.h"

#include <Network.h>

#include "log.h"
#include "net_http.h"
#include "net_lte.h"
#include "net_wifi.h"

namespace
{
NetMode active_mode = MODE_OFF;

void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info)
{
  (void)info;
  switch (event)
  {
    case ARDUINO_EVENT_PPP_CONNECTED:    LOGI("NET", "PPP connected"); break;
    case ARDUINO_EVENT_PPP_GOT_IP:       LOGI("NET", "PPP got IP"); break;
    case ARDUINO_EVENT_PPP_LOST_IP:      LOGI("NET", "PPP lost IP"); break;
    case ARDUINO_EVENT_PPP_DISCONNECTED: LOGI("NET", "PPP disconnected"); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      LOGV("NET", "Wi-Fi disconnected, reason %u", info.wifi_sta_disconnected.reason);
      break;
    default: break;
  }
}
}  // namespace

void netInit(void)
{
  Network.onEvent(onNetworkEvent);
  wifiInit();
  lteInit();
  httpInit();
}

void netTask(void)
{
  wifiTask();
}

uint8_t netOff(void)
{
  httpClose();
  wifiOff();
  lteStop();
  active_mode = MODE_OFF;
  return ST_OK;
}

uint8_t netWifiScan(void)
{
  if (active_mode == MODE_LTE)
  {
    httpClose();
    lteStop();
  }
  active_mode = MODE_WIFI;
  return wifiScanStart();
}

uint8_t netWifiJoin(const char *ssid, const char *pw)
{
  httpClose();
  if (active_mode == MODE_LTE)
  {
    lteStop();
  }
  active_mode = MODE_WIFI;
  wifiJoin(ssid, pw);
  return ST_OK;
}

uint8_t netLteJoin(const char *apn)
{
  if (active_mode == MODE_LTE && lteIsActiveWithApn(apn))
  {
    return ST_OK;  // already connecting/connected with this APN
  }
  httpClose();
  wifiOff();
  active_mode = MODE_LTE;
  lteStart(apn);
  return ST_OK;
}

void netGetStatus(NetStatusInfo *st)
{
  memset(st, 0, sizeof(*st));
  st->mode = active_mode;
  if (active_mode == MODE_WIFI)
  {
    wifiGetStatus(st);
  }
  else if (active_mode == MODE_LTE)
  {
    lteGetStatus(st);
  }
}

bool netIsConnected(void)
{
  if (active_mode == MODE_WIFI)
  {
    return wifiIsConnected();
  }
  if (active_mode == MODE_LTE)
  {
    return lteIsConnected();
  }
  return false;
}

const char *netModeName(uint8_t mode)
{
  switch (mode)
  {
    case MODE_OFF:  return "off";
    case MODE_WIFI: return "wifi";
    case MODE_LTE:  return "lte";
  }
  return "?";
}

const char *netStateName(uint8_t state)
{
  switch (state)
  {
    case STATE_IDLE:       return "idle";
    case STATE_CONNECTING: return "connecting";
    case STATE_CONNECTED:  return "connected";
    case STATE_FAILED:     return "failed";
  }
  return "?";
}
