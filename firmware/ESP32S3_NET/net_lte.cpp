#include "net_lte.h"

#include <PPP.h>

#include "config.h"
#include "log.h"

#if !defined(ARDUINO_HAS_ESP_MODEM) || !CONFIG_LWIP_PPP_SUPPORT
#error "This core build has no PPP/esp_modem support"
#endif

namespace
{
// Quectel EC2x/EG9x speak the 3GPP standard set used by the generic DCE
// (AT+CGDCONT + ATD*99#). PPP_MODEM_BG96 is the other Quectel option.
constexpr ppp_modem_model_t LTE_MODEM_MODEL  = PPP_MODEM_GENERIC;
constexpr uint32_t          LTE_OFF_HOLD_MS  = 1500;   // power-off time when restarting
constexpr uint32_t          LTE_TASK_TICK_MS = 100;
constexpr uint32_t          LTE_RSSI_POLL_MS = 15000;  // CMUX only
constexpr uint32_t          LTE_TASK_STACK   = 8192;

#if LTE_USE_CMUX
constexpr esp_modem_dce_mode_t LTE_ONLINE_MODE = ESP_MODEM_MODE_CMUX;
#else
constexpr esp_modem_dce_mode_t LTE_ONLINE_MODE = ESP_MODEM_MODE_DATA;
#endif

enum class Step : uint8_t
{
  IDLE,
  POWER_WAIT,   // modem booting after power-on
  MODEM_START,  // PPP.begin() until the modem answers AT
  ATTACH_WAIT,  // SIM ready + network attach (AT+CGATT?)
  DIAL,         // switch to PPP data mode
  IP_WAIT,
  CONNECTED,
  FAILED,
};

const char *stepName(Step s)
{
  switch (s)
  {
    case Step::IDLE:        return "idle";
    case Step::POWER_WAIT:  return "power on";
    case Step::MODEM_START: return "modem start";
    case Step::ATTACH_WAIT: return "attach";
    case Step::DIAL:        return "dial";
    case Step::IP_WAIT:     return "wait IP";
    case Step::CONNECTED:   return "connected";
    case Step::FAILED:      return "failed";
  }
  return "?";
}

struct Published
{
  Step    step;
  int8_t  rssi;
  uint8_t ip[4];
  char    apn[LTE_APN_MAX + 1];
  char    detail[STATUS_DETAIL_MAX + 1];
};

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// loop() -> task requests, guarded by mux
bool req_start = false;
bool req_stop  = false;
char req_apn[LTE_APN_MAX + 1];

// task -> loop() snapshot, guarded by mux
Published pub = {};

// task-private state
Step        step        = Step::IDLE;
bool        powered     = false;
bool        ppp_started = false;
bool        sim_ok      = false;
char        apn[LTE_APN_MAX + 1];
char        oper[33];
int8_t      rssi_dbm    = 0;
uint8_t     ip[4]       = {0, 0, 0, 0};
const char *wait_reason = "";
char        fail_reason[STATUS_DETAIL_MAX + 1];
uint32_t    t_start     = 0;
uint32_t    t_step      = 0;
uint32_t    t_lost      = 0;
uint32_t    t_rssi      = 0;

int8_t csqToDbm(int csq)
{
  if (csq < 0 || csq > 31)
  {
    return 0;  // 99 = unknown
  }
  return (int8_t)(-113 + 2 * csq);
}

void publish(void)
{
  Published p = {};
  p.step      = step;
  p.rssi      = rssi_dbm;
  memcpy(p.ip, ip, sizeof(p.ip));
  strlcpy(p.apn, apn, sizeof(p.apn));

  if (step == Step::FAILED)
  {
    strlcpy(p.detail, fail_reason, sizeof(p.detail));
  }
  else if (step == Step::CONNECTED && oper[0] != '\0')
  {
    strlcpy(p.detail, oper, sizeof(p.detail));  // "operator/apn", truncated to fit
    strlcat(p.detail, "/", sizeof(p.detail));
    strlcat(p.detail, apn, sizeof(p.detail));
  }
  else if (step != Step::IDLE)
  {
    strlcpy(p.detail, apn, sizeof(p.detail));
  }

  portENTER_CRITICAL(&mux);
  // A newer request from loop() already set the visible state; don't overwrite it.
  if (!req_start && !req_stop)
  {
    pub = p;
  }
  portEXIT_CRITICAL(&mux);
}

void setStep(Step s)
{
  if (step != s)
  {
    step = s;
    LOGI("LTE", "-> %s (%lus)", stepName(s), (unsigned long)((millis() - t_start) / 1000));
  }
  publish();
}

void shutdownModem(void)
{
  if (ppp_started)
  {
    PPP.end();
    ppp_started = false;
  }
  digitalWrite(LTE_PWR_PIN, LOW);
  powered = false;
  sim_ok  = false;
  memset(ip, 0, sizeof(ip));
}

void fail(const char *reason)
{
  strlcpy(fail_reason, reason, sizeof(fail_reason));
  LOGI("LTE", "failed: %s", reason);
  shutdownModem();
  setStep(Step::FAILED);
}

bool requestPending(void)
{
  portENTER_CRITICAL(&mux);
  bool pending = req_start || req_stop;
  portEXIT_CRITICAL(&mux);
  return pending;
}

void startSequence(const char *new_apn)
{
  bool was_powered = powered;
  shutdownModem();
  step = Step::IDLE;
  if (was_powered)
  {
    vTaskDelay(pdMS_TO_TICKS(LTE_OFF_HOLD_MS));
    if (requestPending())
    {
      return;  // superseded while the modem was off
    }
  }

  strlcpy(apn, new_apn, sizeof(apn));
  oper[0]     = '\0';
  rssi_dbm    = 0;
  wait_reason = "modem no response";
  t_start     = millis();
  t_step      = t_start;

  digitalWrite(LTE_PWR_PIN, HIGH);
  powered = true;
  LOGI("LTE", "start apn='%s'", apn);
  setStep(Step::POWER_WAIT);
}

bool checkSim(void)
{
  String resp;
  bool   ok = PPP.cmd("AT+CPIN?", resp, 3000);
  if (ok && resp.indexOf("READY") >= 0)
  {
    sim_ok = true;
    return true;
  }
  if (resp.indexOf("PIN") >= 0 || resp.indexOf("PUK") >= 0)
  {
    wait_reason = "SIM locked";
  }
  else
  {
    wait_reason = ok ? "SIM not ready" : "no SIM";
  }
  LOGV("LTE", "CPIN: %s", resp.c_str());
  return false;
}

void runStep(void)
{
  uint32_t now = millis();

  /* The join limit applies only while joining; IDLE (never started or stopped) and FAILED stay put,
   * otherwise fail() would repeat on every tick once t_start is old. */
  bool joining = step != Step::IDLE && step != Step::FAILED && step != Step::CONNECTED;
  if (joining && now - t_start > LTE_JOIN_TIMEOUT_MS)
  {
    fail(wait_reason);
    return;
  }

  switch (step)
  {
    case Step::POWER_WAIT:
      if (now - t_step >= LTE_BOOT_MS)
      {
        t_step = now - LTE_BEGIN_RETRY_MS;  // first attempt right away
        setStep(Step::MODEM_START);
      }
      break;

    case Step::MODEM_START:
      if (now - t_step >= LTE_BEGIN_RETRY_MS)
      {
        PPP.setApn(apn);
        PPP.setPins(LTE_TX_PIN, LTE_RX_PIN);  // PPP.end() releases the pins, set them every time
        if (PPP.begin(LTE_MODEM_MODEL, LTE_UART_NUM, LTE_BAUD))
        {
          ppp_started = true;
          LOGI("LTE", "modem '%s'", PPP.moduleName().c_str());
          wait_reason = "not attached";
          t_step      = millis() - LTE_ATTACH_POLL_MS;
          setStep(Step::ATTACH_WAIT);
        }
        else
        {
          LOGV("LTE", "modem not answering yet");
          t_step = millis();
        }
      }
      break;

    case Step::ATTACH_WAIT:
      if (now - t_step >= LTE_ATTACH_POLL_MS)
      {
        t_step = now;
        if (!sim_ok && !checkSim())
        {
          publish();
          break;
        }
        rssi_dbm = csqToDbm(PPP.RSSI());
        if (PPP.attached())
        {
          strlcpy(oper, PPP.operatorName().c_str(), sizeof(oper));
          LOGI("LTE", "attached oper='%s' rssi=%d", oper, rssi_dbm);
          wait_reason = "dial failed";
          t_step      = millis() - LTE_DIAL_RETRY_MS;
          setStep(Step::DIAL);
        }
        else
        {
          wait_reason = "not attached";
          publish();
        }
      }
      break;

    case Step::DIAL:
      if (now - t_step >= LTE_DIAL_RETRY_MS)
      {
        if (PPP.mode(LTE_ONLINE_MODE))
        {
          wait_reason = "no IP";
          setStep(Step::IP_WAIT);
        }
        else
        {
          LOGI("LTE", "dial failed, retrying");
        }
        t_step = millis();
      }
      break;

    case Step::IP_WAIT:
      if (PPP.hasIP())
      {
        IPAddress a = PPP.localIP();
        for (int i = 0; i < 4; i++)
        {
          ip[i] = a[i];
        }
        t_lost = 0;
        t_rssi = now;
        LOGI("LTE", "ip=%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        setStep(Step::CONNECTED);
      }
      break;

    case Step::CONNECTED:
      if (!PPP.hasIP())
      {
        if (t_lost == 0)
        {
          t_lost = now;
        }
        else if (now - t_lost > LTE_LINK_LOST_MS)
        {
          fail("link lost");
        }
        break;
      }
      t_lost = 0;
#if LTE_USE_CMUX
      if (now - t_rssi >= LTE_RSSI_POLL_MS)
      {
        t_rssi   = now;
        rssi_dbm = csqToDbm(PPP.RSSI());
        publish();
      }
#endif
      break;

    case Step::IDLE:
    case Step::FAILED:
      break;
  }
}

void lteTaskFn(void *arg)
{
  (void)arg;
  char new_apn[LTE_APN_MAX + 1];

  for (;;)
  {
    portENTER_CRITICAL(&mux);
    bool do_stop  = req_stop;
    bool do_start = req_start;
    if (do_start)
    {
      memcpy(new_apn, req_apn, sizeof(new_apn));
    }
    req_stop  = false;
    req_start = false;
    portEXIT_CRITICAL(&mux);

    if (do_start)
    {
      startSequence(new_apn);
    }
    else if (do_stop)
    {
      bool was_active = powered || ppp_started;
      shutdownModem();
      step = Step::IDLE;
      publish();
      if (was_active)
      {
        LOGI("LTE", "off");
      }
    }

    runStep();
    vTaskDelay(pdMS_TO_TICKS(LTE_TASK_TICK_MS));
  }
}
}  // namespace

void lteInit(void)
{
  pinMode(LTE_PWR_PIN, OUTPUT);
  digitalWrite(LTE_PWR_PIN, LOW);
  xTaskCreatePinnedToCore(lteTaskFn, "lte", LTE_TASK_STACK, nullptr, 1, nullptr, 0);
}

void lteStart(const char *new_apn)
{
  portENTER_CRITICAL(&mux);
  strlcpy(req_apn, new_apn, sizeof(req_apn));
  req_start = true;
  req_stop  = false;
  pub       = {};
  pub.step  = Step::POWER_WAIT;
  strlcpy(pub.apn, new_apn, sizeof(pub.apn));
  strlcpy(pub.detail, new_apn, sizeof(pub.detail));
  portEXIT_CRITICAL(&mux);
}

void lteStop(void)
{
  portENTER_CRITICAL(&mux);
  req_stop  = true;
  req_start = false;
  pub       = {};
  pub.step  = Step::IDLE;
  portEXIT_CRITICAL(&mux);

  // Cut modem power right away; the task tears PPP down when its current call returns.
  digitalWrite(LTE_PWR_PIN, LOW);
}

bool lteIsActiveWithApn(const char *want_apn)
{
  portENTER_CRITICAL(&mux);
  bool active = !req_stop && pub.step != Step::IDLE && pub.step != Step::FAILED && strcmp(pub.apn, want_apn) == 0;
  portEXIT_CRITICAL(&mux);
  return active;
}

bool lteIsConnected(void)
{
  portENTER_CRITICAL(&mux);
  bool connected = !req_stop && !req_start && pub.step == Step::CONNECTED;
  portEXIT_CRITICAL(&mux);
  return connected;
}

void lteGetStatus(NetStatusInfo *st)
{
  Published p;
  portENTER_CRITICAL(&mux);
  p = pub;
  portEXIT_CRITICAL(&mux);

  switch (p.step)
  {
    case Step::IDLE:      st->state = STATE_IDLE; break;
    case Step::CONNECTED: st->state = STATE_CONNECTED; break;
    case Step::FAILED:    st->state = STATE_FAILED; break;
    default:              st->state = STATE_CONNECTING; break;
  }
  st->rssi = p.rssi;
  if (p.step == Step::CONNECTED)
  {
    memcpy(st->ip, p.ip, sizeof(st->ip));
  }
  strlcpy(st->detail, p.detail, sizeof(st->detail));
}
