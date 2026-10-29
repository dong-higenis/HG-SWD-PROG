#ifndef NET_H
#define NET_H
#include "net_link.h"
#include "net_store.h"

#define NET_AP_MAX 16

typedef enum
{
  NET_STATE_IDLE,
  NET_STATE_CONNECTING,
  NET_STATE_CONNECTED,
  NET_STATE_FAILED
} net_state_t;

typedef struct
{
  uint8_t mode;  /* net_mode_t */
  uint8_t state; /* net_state_t */
  int8_t  rssi;  /* dBm, 0 when unknown. */
  uint8_t ip[4];
  char    detail[48]; /* SSID, APN/operator, or the failure reason. */
} net_info_t;

typedef struct
{
  int8_t rssi;
  bool   secure;
  char   ssid[NET_SSID_SIZE];
} net_ap_t;

/* LTE APN presets (Korean carriers as tested on the HG-ESP32S3-LTE board). */
typedef struct
{
  const char *label;
  const char *apn;
} net_apn_t;
extern const net_apn_t net_apns[];
extern const unsigned  net_apn_count;

/* All calls block for at most their protocol timeout; the caller owns the CLI/SD resource lock. */
bool        netIsPresent(void); /* ESP answered HELLO recently. */
const char *netEspVersion(void);
uint8_t     netHello(void);
uint8_t     netInfo(net_info_t *info);
const net_info_t *netInfoCached(void); /* Last STATUS; refreshed by netUpdate() about once a second. */
bool        netIsConnected(void);         /* From the last STATUS. */
uint8_t     netOff(void);
uint8_t     netWifiScan(void);
uint8_t     netWifiList(net_ap_t *list, unsigned max, unsigned *count); /* Strongest first. */
uint8_t     netWifiJoin(const char *ssid, const char *password);
uint8_t     netLteJoin(const char *apn);
uint8_t     netHttpGet(const char *url, uint16_t *code, uint32_t *length);
uint8_t     netHttpRead(void *data, uint16_t max, uint16_t *size);
uint8_t     netHttpClose(void);
const char *netStatusText(uint8_t status);

/* Settings kept on the SD card; reloaded when the card changes. */
net_store_t *netSettings(void);
void         netSettingsReload(bool mounted);
bool         netSettingsSave(void);

/* Called while a request waits for the ESP (up to 25 s for a TLS handshake); the UI uses it to keep
 * animating. The hook must not call back into the network. */
void netSetIdleHook(void (*hook)(void));

/* Called from the main loop: finds the ESP and, once per power-up or card change, rejoins the saved network. */
void netUpdate(void);
#endif
