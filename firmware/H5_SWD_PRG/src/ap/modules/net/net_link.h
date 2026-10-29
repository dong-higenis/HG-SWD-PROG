#ifndef NET_LINK_H
#define NET_LINK_H
#include <stdbool.h>
#include <stdint.h>

/* H5 side of the ESP32-S3 link; frame format and commands: firmware/ESP32S3_NET/PROTOCOL.md. */
#define NET_LINK_BAUD        921600
#define NET_LINK_PAYLOAD_MAX 1100
#define NET_LINK_FRAME_MAX   (NET_LINK_PAYLOAD_MAX + 8)
#define NET_HTTP_READ_MAX    1024 /* HTTP_READ max; the ESP answers BAD_REQUEST above it. */

typedef enum
{
  NET_CMD_HELLO      = 0x01,
  NET_CMD_STATUS     = 0x02,
  NET_CMD_OFF        = 0x03,
  NET_CMD_WIFI_SCAN  = 0x10,
  NET_CMD_WIFI_LIST  = 0x11,
  NET_CMD_WIFI_JOIN  = 0x12,
  NET_CMD_LTE_JOIN   = 0x20,
  NET_CMD_HTTP_GET   = 0x30,
  NET_CMD_HTTP_READ  = 0x31,
  NET_CMD_HTTP_CLOSE = 0x32
} net_cmd_t;

typedef enum
{
  NET_ST_OK            = 0,
  NET_ST_BUSY          = 1,
  NET_ST_ERROR         = 2,
  NET_ST_BAD_REQUEST   = 3,
  NET_ST_NOT_CONNECTED = 4,
  NET_ST_HTTP          = 5,
  NET_ST_TIMEOUT       = 6,
  NET_ST_UNKNOWN_CMD   = 7,
  NET_ST_NO_LINK       = 0xFF /* H5 only: no valid response after the retries. */
} net_status_t;

/* Byte-wise frame receiver. */
typedef struct
{
  uint16_t pos;
  uint16_t len;
  uint8_t  frame[NET_LINK_FRAME_MAX];
} net_frame_rx_t;

/* Byte access to the UART; the firmware binds HW_UART_CH_NET, host tests bind a simulated ESP. */
typedef struct
{
  uint32_t (*available)(void);
  uint8_t (*read)(void);
  uint32_t (*write)(const uint8_t *data, uint32_t size);
  uint32_t (*millis)(void);
  void (*idle)(void); /* Called while waiting; yields to other tasks. */
} net_link_io_t;

uint16_t netLinkCrc(const uint8_t *data, uint32_t size);
uint32_t netLinkEncode(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t size, uint8_t *frame);
void     netLinkRxReset(net_frame_rx_t *rx);
/* True once a complete frame with a valid CRC has arrived; frame[2]=cmd, [3]=seq, payload at frame + 6. */
bool     netLinkRxFeed(net_frame_rx_t *rx, uint8_t byte);

void     netLinkBind(const net_link_io_t *io);
/* The ESP answers a repeated cmd+seq from its cache, so a rebooted H5 must not restart at the same seq. */
void     netLinkSeed(uint8_t seq);
/* One request/response exchange, retried with the same sequence number. Returns the response status;
 * the response data (after the status byte) is copied to data, its size to *size. */
uint8_t  netLinkCall(uint8_t cmd, const void *request, uint16_t request_size, void *data, uint16_t data_max,
                     uint16_t *size, uint32_t timeout_ms, unsigned attempts);
#endif
