#ifndef NET_STORE_H
#define NET_STORE_H
#include <stdbool.h>
#include <stdint.h>

/* Network settings on the SD card: /net/net.cfg. Wi-Fi passwords are stored only after a network
 * connected once. The record is XOR-scrambled with a key from the H5's unique ID, so a copied card
 * does not show passwords in plain text; this is obfuscation, not strong encryption. */
#define NET_STORE_DIR       "/net"
#define NET_STORE_PATH      "/net/net.cfg"
#define NET_STORE_NETWORKS  8
#define NET_SSID_SIZE       33
#define NET_PASSWORD_SIZE   65
#define NET_APN_SIZE        64
#define NET_STORE_SIZE      (12 + NET_APN_SIZE + NET_SSID_SIZE + NET_STORE_NETWORKS * (NET_SSID_SIZE + NET_PASSWORD_SIZE))

typedef enum
{
  NET_MODE_OFF,
  NET_MODE_WIFI,
  NET_MODE_LTE
} net_mode_t;

typedef struct
{
  char ssid[NET_SSID_SIZE];
  char password[NET_PASSWORD_SIZE];
} net_known_t;

typedef struct
{
  uint8_t     mode; /* net_mode_t last connected, rejoined at power-up. */
  char        apn[NET_APN_SIZE];
  char        ssid[NET_SSID_SIZE]; /* Last Wi-Fi network. */
  uint8_t     count;
  net_known_t known[NET_STORE_NETWORKS]; /* Most recently connected first. */
} net_store_t;

void        netStoreSetKey(const uint8_t key[12]);
/* Codec only; size is NET_STORE_SIZE. */
void        netStoreEncode(const net_store_t *store, uint8_t *record);
bool        netStoreDecode(const uint8_t *record, net_store_t *store);
bool        netStoreLoad(net_store_t *store); /* Zeroed when missing or damaged. */
bool        netStoreSave(const net_store_t *store);
const char *netStorePassword(const net_store_t *store, const char *ssid);
void        netStoreRemember(net_store_t *store, const char *ssid, const char *password);
#endif
