#include "net_wifi.h"

#include <WiFi.h>

#include "config.h"
#include "log.h"

namespace
{
enum class JoinState : uint8_t
{
  IDLE,
  PENDING,  // requested; started by wifiTask() once no scan is running
  CONNECTING,
  CONNECTED,
  FAILED,
};

enum class ScanPhase : uint8_t
{
  NONE,
  PENDING,
  RUNNING,
  DONE,
  ERROR,
};

bool        radio_on   = false;
ScanPhase   scan_phase = ScanPhase::NONE;
WifiApEntry ap_list[WIFI_LIST_MAX];
uint8_t     ap_count   = 0;

JoinState join_state = JoinState::IDLE;
char      join_ssid[WIFI_SSID_MAX + 1];
char      join_pw[WIFI_PW_MAX + 1];
uint32_t  join_start_ms = 0;
char      fail_reason[STATUS_DETAIL_MAX + 1];

// Written from the Arduino event task, read in loop().
volatile bool    ev_tracking      = false;
volatile uint8_t ev_disconnects   = 0;
volatile uint8_t ev_auth_fails    = 0;
volatile uint8_t ev_no_ap         = 0;
volatile uint8_t ev_last_reason   = 0;

bool isAuthReason(uint8_t r)
{
  return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

bool isNoApReason(uint8_t r)
{
  return r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
         r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD || r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

void onStaDisconnected(arduino_event_id_t event, arduino_event_info_t info)
{
  (void)event;
  if (!ev_tracking)
  {
    return;
  }
  uint8_t r = info.wifi_sta_disconnected.reason;
  if (r == WIFI_REASON_ASSOC_LEAVE)
  {
    return;  // our own WiFi.disconnect()
  }
  ev_last_reason = r;
  if (ev_disconnects < 255)
  {
    ev_disconnects = ev_disconnects + 1;
  }
  if (isAuthReason(r) && ev_auth_fails < 255)
  {
    ev_auth_fails = ev_auth_fails + 1;
  }
  if (isNoApReason(r) && ev_no_ap < 255)
  {
    ev_no_ap = ev_no_ap + 1;
  }
}

void resetJoinEvents(void)
{
  ev_disconnects = 0;
  ev_auth_fails  = 0;
  ev_no_ap       = 0;
  ev_last_reason = 0;
}

void ensureRadio(void)
{
  if (radio_on)
  {
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep costs a lot of download throughput
  WiFi.setAutoReconnect(true);
  radio_on = true;
}

void failJoin(const char *reason)
{
  ev_tracking = false;
  join_state  = JoinState::FAILED;
  strlcpy(fail_reason, reason, sizeof(fail_reason));
  WiFi.disconnect();  // stop the core's auto-reconnect so scans keep working
  LOGI("WIFI", "join '%s' failed: %s (last reason %u)", join_ssid, reason, ev_last_reason);
}

const char *timeoutReason(void)
{
  if (ev_auth_fails > 0)
  {
    return "wrong password";
  }
  if (ev_no_ap > 0)
  {
    return "no AP";
  }
  return "timeout";
}

// Keeps the strongest entry per SSID, drops hidden SSIDs, caps at WIFI_LIST_MAX.
void addAp(const wifi_ap_record_t *rec)
{
  size_t len = strnlen((const char *)rec->ssid, WIFI_SSID_MAX);
  if (len == 0)
  {
    return;
  }

  int8_t  rssi = rec->rssi;
  uint8_t auth = rec->authmode == WIFI_AUTH_OPEN ? 0 : 1;

  for (uint8_t i = 0; i < ap_count; i++)
  {
    if (ap_list[i].ssid_len == len && memcmp(ap_list[i].ssid, rec->ssid, len) == 0)
    {
      if (rssi > ap_list[i].rssi)
      {
        ap_list[i].rssi = rssi;
        ap_list[i].auth = auth;
      }
      return;
    }
  }

  uint8_t slot = ap_count;
  if (ap_count >= WIFI_LIST_MAX)
  {
    slot = 0;
    for (uint8_t i = 1; i < ap_count; i++)
    {
      if (ap_list[i].rssi < ap_list[slot].rssi)
      {
        slot = i;
      }
    }
    if (rssi <= ap_list[slot].rssi)
    {
      return;
    }
  }
  else
  {
    ap_count++;
  }

  WifiApEntry &e = ap_list[slot];
  e.rssi         = rssi;
  e.auth         = auth;
  e.ssid_len     = (uint8_t)len;
  memcpy(e.ssid, rec->ssid, len);
  e.ssid[len] = '\0';
}

void collectScan(int16_t n)
{
  ap_count = 0;
  for (int16_t i = 0; i < n; i++)
  {
    const wifi_ap_record_t *rec = (const wifi_ap_record_t *)WiFi.getScanInfoByIndex(i);
    if (rec != nullptr)
    {
      addAp(rec);
    }
  }
  WiFi.scanDelete();

  // Insertion sort, RSSI descending.
  for (uint8_t i = 1; i < ap_count; i++)
  {
    WifiApEntry tmp = ap_list[i];
    int8_t      j   = (int8_t)i - 1;
    while (j >= 0 && ap_list[j].rssi < tmp.rssi)
    {
      ap_list[j + 1] = ap_list[j];
      j--;
    }
    ap_list[j + 1] = tmp;
  }

  scan_phase = ScanPhase::DONE;
  LOGI("WIFI", "scan done: %d raw, %u listed", n, ap_count);
}

void scanStep(void)
{
  if (scan_phase == ScanPhase::PENDING)
  {
    ensureRadio();
    int16_t r = WiFi.scanNetworks(true, false);
    if (r == WIFI_SCAN_RUNNING)
    {
      scan_phase = ScanPhase::RUNNING;
    }
    else if (r >= 0)
    {
      collectScan(r);
    }
    else
    {
      scan_phase = ScanPhase::ERROR;
      LOGI("WIFI", "scan start failed (%d)", r);
    }
  }
  else if (scan_phase == ScanPhase::RUNNING)
  {
    int16_t r = WiFi.scanComplete();
    if (r >= 0)
    {
      collectScan(r);
    }
    else if (r != WIFI_SCAN_RUNNING)
    {
      WiFi.scanDelete();
      scan_phase = ScanPhase::ERROR;
      LOGI("WIFI", "scan failed (%d)", r);
    }
  }
}

void joinStep(void)
{
  switch (join_state)
  {
    case JoinState::PENDING:
    {
      if (scan_phase == ScanPhase::PENDING || scan_phase == ScanPhase::RUNNING)
      {
        break;  // the driver cannot connect while scanning
      }
      ensureRadio();
      WiFi.disconnect();
      resetJoinEvents();
      ev_tracking   = true;
      join_start_ms = millis();
      join_state    = JoinState::CONNECTING;
      LOGI("WIFI", "joining '%s'", join_ssid);
      wl_status_t r = WiFi.begin(join_ssid, join_pw[0] ? join_pw : nullptr);
      if (r == WL_CONNECT_FAILED)
      {
        failJoin("bad parameters");
      }
    }
    break;

    case JoinState::CONNECTING:
    {
      wl_status_t st = WiFi.status();
      if (st == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0)
      {
        join_state = JoinState::CONNECTED;
        LOGI("WIFI", "connected '%s' ip=%s rssi=%d", join_ssid, WiFi.localIP().toString().c_str(), WiFi.RSSI());
      }
      else if (ev_auth_fails >= WIFI_AUTH_FAIL_LIMIT || (st == WL_CONNECT_FAILED && ev_disconnects > 0))
      {
        failJoin(ev_last_reason == WIFI_REASON_ASSOC_FAIL ? "assoc failed" : "wrong password");
      }
      else if (millis() - join_start_ms > WIFI_JOIN_TIMEOUT_MS)
      {
        failJoin(timeoutReason());
      }
    }
    break;

    case JoinState::CONNECTED:
      if (WiFi.status() != WL_CONNECTED)
      {
        // The core auto-reconnects; give it a fresh join window.
        resetJoinEvents();
        join_start_ms = millis();
        join_state    = JoinState::CONNECTING;
        LOGI("WIFI", "link lost (reason %u), reconnecting", ev_last_reason);
      }
      break;

    case JoinState::IDLE:
    case JoinState::FAILED:
      break;
  }
}
}  // namespace

void wifiInit(void)
{
  WiFi.persistent(false);       // never write credentials to NVS
  WiFi.setScanTimeout(15000);  // a full active scan takes ~4 s; don't report BUSY for the 60 s default
  WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

void wifiTask(void)
{
  scanStep();
  joinStep();
}

void wifiOff(void)
{
  ev_tracking = false;
  join_state  = JoinState::IDLE;
  scan_phase  = ScanPhase::NONE;
  ap_count    = 0;
  join_ssid[0] = '\0';
  join_pw[0]   = '\0';

  if (radio_on)
  {
    WiFi.scanDelete();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    radio_on = false;
    LOGI("WIFI", "off");
  }
}

uint8_t wifiScanStart(void)
{
  if (join_state == JoinState::PENDING || join_state == JoinState::CONNECTING)
  {
    return ST_BUSY;
  }
  if (scan_phase != ScanPhase::RUNNING)
  {
    scan_phase = ScanPhase::PENDING;
  }
  return ST_OK;
}

uint8_t wifiScanState(void)
{
  switch (scan_phase)
  {
    case ScanPhase::PENDING:
    case ScanPhase::RUNNING: return APSCAN_BUSY;
    case ScanPhase::DONE:    return APSCAN_DONE;
    case ScanPhase::ERROR:   return APSCAN_ERROR;
    default:                 return APSCAN_NONE;
  }
}

const WifiApEntry *wifiScanResults(uint8_t *count)
{
  *count = ap_count;
  return ap_list;
}

void wifiJoin(const char *ssid, const char *pw)
{
  strlcpy(join_ssid, ssid, sizeof(join_ssid));
  strlcpy(join_pw, pw, sizeof(join_pw));
  fail_reason[0] = '\0';
  ev_tracking    = false;
  join_state     = JoinState::PENDING;
}

bool wifiIsConnected(void)
{
  return join_state == JoinState::CONNECTED && WiFi.status() == WL_CONNECTED;
}

void wifiGetStatus(NetStatusInfo *st)
{
  switch (join_state)
  {
    case JoinState::PENDING:
    case JoinState::CONNECTING:
      st->state = STATE_CONNECTING;
      strlcpy(st->detail, join_ssid, sizeof(st->detail));
      break;

    case JoinState::CONNECTED:
    {
      st->state     = STATE_CONNECTED;
      st->rssi      = (int8_t)WiFi.RSSI();
      IPAddress ip  = WiFi.localIP();
      for (int i = 0; i < 4; i++)
      {
        st->ip[i] = ip[i];
      }
      strlcpy(st->detail, join_ssid, sizeof(st->detail));
    }
    break;

    case JoinState::FAILED:
      st->state = STATE_FAILED;
      strlcpy(st->detail, fail_reason, sizeof(st->detail));
      break;

    case JoinState::IDLE:
      st->state = STATE_IDLE;
      break;
  }
}
