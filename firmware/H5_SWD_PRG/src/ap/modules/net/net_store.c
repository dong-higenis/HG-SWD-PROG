#include "net_store.h"
#include "fatfs.h"
#include <string.h>

#define NET_STORE_TMP "/net/net.tmp"

static uint8_t store_key[12];

static uint32_t netStoreCrc(const uint8_t *data, uint32_t size)
{
  uint32_t crc = UINT32_MAX;
  for (uint32_t n = 0; n < size; n++)
  {
    crc ^= data[n];
    for (unsigned bit = 0; bit < 8; bit++)
    {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
    }
  }
  return ~crc;
}

void netStoreSetKey(const uint8_t key[12])
{
  memcpy(store_key, key, sizeof(store_key));
}

/* xorshift32 keystream seeded from the device key; applying it twice restores the data. */
static void netStoreScramble(uint8_t *data, uint32_t size)
{
  uint32_t x = netStoreCrc(store_key, sizeof(store_key)) | 1U;
  for (uint32_t i = 0; i < size; i++)
  {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    data[i] ^= (uint8_t)x;
  }
}

static void netStorePut(uint8_t **p, const char *s, uint32_t size)
{
  memset(*p, 0, size);
  strncpy((char *)*p, s, size - 1);
  *p += size;
}

static bool netStoreGet(const uint8_t **p, char *s, uint32_t size)
{
  memcpy(s, *p, size);
  *p += size;
  return memchr(s, 0, size) != NULL;
}

/* Layout: "NET1", mode, count, 2 spare, apn, ssid, known[], then the CRC32 of the plain record. Everything from
 * byte 8 is scrambled, so a card moved to another programmer fails the CRC and reads as empty. */
void netStoreEncode(const net_store_t *store, uint8_t *record)
{
  uint8_t *p = record;
  memset(record, 0, NET_STORE_SIZE);
  memcpy(p, "NET1", 4);
  p[4] = store->mode;
  p[5] = store->count;
  p += 8;
  netStorePut(&p, store->apn, NET_APN_SIZE);
  netStorePut(&p, store->ssid, NET_SSID_SIZE);
  for (unsigned i = 0; i < NET_STORE_NETWORKS; i++)
  {
    netStorePut(&p, i < store->count ? store->known[i].ssid : "", NET_SSID_SIZE);
    netStorePut(&p, i < store->count ? store->known[i].password : "", NET_PASSWORD_SIZE);
  }
  uint32_t crc = netStoreCrc(record, (uint32_t)(p - record));
  memcpy(p, &crc, 4);
  netStoreScramble(record + 8, NET_STORE_SIZE - 8);
}

bool netStoreDecode(const uint8_t *record, net_store_t *store)
{
  static uint8_t copy[NET_STORE_SIZE];
  uint32_t       body = NET_STORE_SIZE - 4; /* Up to the CRC. */
  uint32_t       crc;
  memset(store, 0, sizeof(*store));
  memcpy(copy, record, NET_STORE_SIZE);
  netStoreScramble(copy + 8, NET_STORE_SIZE - 8);
  memcpy(&crc, copy + body, 4);
  if (memcmp(copy, "NET1", 4) || crc != netStoreCrc(copy, body) || copy[4] > NET_MODE_LTE ||
      copy[5] > NET_STORE_NETWORKS)
  {
    return false;
  }
  net_store_t     s = {0};
  const uint8_t  *p = copy + 8;
  bool            ok = true;
  s.mode             = copy[4];
  s.count            = copy[5];
  ok &= netStoreGet(&p, s.apn, NET_APN_SIZE);
  ok &= netStoreGet(&p, s.ssid, NET_SSID_SIZE);
  for (unsigned i = 0; i < NET_STORE_NETWORKS; i++)
  {
    ok &= netStoreGet(&p, s.known[i].ssid, NET_SSID_SIZE);
    ok &= netStoreGet(&p, s.known[i].password, NET_PASSWORD_SIZE);
  }
  if (ok)
  {
    *store = s;
  }
  return ok;
}

bool netStoreLoad(net_store_t *store)
{
  static uint8_t record[NET_STORE_SIZE];
  memset(store, 0, sizeof(*store));
  FIL  file;
  UINT got = 0;
  if (f_open(&file, NET_STORE_PATH, FA_READ) != FR_OK)
  {
    return false;
  }
  bool    ok     = f_size(&file) == NET_STORE_SIZE && f_read(&file, record, NET_STORE_SIZE, &got) == FR_OK &&
                   got == NET_STORE_SIZE;
  FRESULT closed = f_close(&file);
  return ok && closed == FR_OK && netStoreDecode(record, store);
}

/* Written beside the old file and renamed over it, so a pulled card keeps the previous settings. */
bool netStoreSave(const net_store_t *store)
{
  static uint8_t record[NET_STORE_SIZE];
  netStoreEncode(store, record);
  FRESULT e = f_mkdir(NET_STORE_DIR);
  if (e != FR_OK && e != FR_EXIST)
  {
    return false;
  }
  FIL  file;
  UINT written = 0;
  if (f_open(&file, NET_STORE_TMP, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    return false;
  }
  bool    ok     = f_write(&file, record, NET_STORE_SIZE, &written) == FR_OK && written == NET_STORE_SIZE;
  FRESULT synced = f_sync(&file);
  FRESULT closed = f_close(&file);
  if (!ok || synced != FR_OK || closed != FR_OK)
  {
    f_unlink(NET_STORE_TMP);
    return false;
  }
  e = f_unlink(NET_STORE_PATH);
  if (e != FR_OK && e != FR_NO_FILE)
  {
    return false;
  }
  return f_rename(NET_STORE_TMP, NET_STORE_PATH) == FR_OK;
}

const char *netStorePassword(const net_store_t *store, const char *ssid)
{
  for (unsigned i = 0; i < store->count; i++)
  {
    if (!strcmp(store->known[i].ssid, ssid))
    {
      return store->known[i].password;
    }
  }
  return NULL;
}

void netStoreRemember(net_store_t *store, const char *ssid, const char *password)
{
  net_known_t entry = {0};
  strncpy(entry.ssid, ssid, NET_SSID_SIZE - 1);
  strncpy(entry.password, password, NET_PASSWORD_SIZE - 1);
  unsigned last = store->count < NET_STORE_NETWORKS ? store->count : NET_STORE_NETWORKS - 1;
  for (unsigned i = 0; i < store->count; i++)
  {
    if (!strcmp(store->known[i].ssid, ssid))
    {
      last = i; /* Already known: move it to the front. */
      break;
    }
  }
  if (last == store->count)
  {
    store->count++;
  }
  memmove(&store->known[1], &store->known[0], last * sizeof(net_known_t));
  store->known[0] = entry;
}
