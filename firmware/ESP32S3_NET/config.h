#ifndef CONFIG_H_
#define CONFIG_H_

#include <Arduino.h>

#define NET_FW_NAME    "ESP32S3_NET"
#define NET_FW_VERSION "1.0.0"

// H5 link (Serial2). GPIO45 is a strapping pin (VDD_SPI select), so it must be
// the ESP *output*; the H5 TX line (idle high) goes to GPIO48. See PROTOCOL.md.
constexpr int      H5_RX_PIN       = 48;
constexpr int      H5_TX_PIN       = 45;
constexpr uint32_t H5_BAUD         = 921600;
constexpr size_t   H5_RX_BUF_SIZE  = 4096;
constexpr size_t   H5_TX_BUF_SIZE  = 2048;

// Quectel LTE modem on UART1 (driven by the core PPP library / esp_modem).
constexpr int      LTE_TX_PIN      = 17;   // ESP TX -> modem RX
constexpr int      LTE_RX_PIN      = 18;   // ESP RX <- modem TX
constexpr int      LTE_PWR_PIN     = 5;    // HIGH = modem power on
constexpr uint8_t  LTE_UART_NUM    = 1;
constexpr int      LTE_BAUD        = 115200;
constexpr uint32_t LTE_BOOT_MS          = 8000;    // wait after power-on before talking AT
constexpr uint32_t LTE_BEGIN_RETRY_MS   = 3000;    // PPP.begin() retry period while the modem boots
constexpr uint32_t LTE_ATTACH_POLL_MS   = 2000;
constexpr uint32_t LTE_DIAL_RETRY_MS    = 3000;
constexpr uint32_t LTE_JOIN_TIMEOUT_MS  = 180000;  // power-on .. got IP
constexpr uint32_t LTE_LINK_LOST_MS     = 3000;    // IP gone this long while connected -> failed
// 0: plain PPP DATA mode (most compatible; RSSI/operator frozen at dial time).
// 1: CMUX (AT commands keep working while online; RSSI refreshed). Untested on Quectel.
#define LTE_USE_CMUX 0

// Wi-Fi
constexpr uint32_t WIFI_JOIN_TIMEOUT_MS   = 20000;
constexpr uint8_t  WIFI_AUTH_FAIL_LIMIT   = 2;     // auth-type disconnects before giving up early
constexpr uint8_t  WIFI_LIST_MAX          = 16;

// HTTP
constexpr uint32_t HTTP_GET_TIMEOUT_MS    = 20000; // connect + TLS + response headers
constexpr uint32_t HTTP_READ_TIMEOUT_MS   = 5000;  // wait for the first body byte
constexpr uint16_t HTTP_READ_MAX          = 1024;
constexpr size_t   HTTP_URL_MAX           = 255;

// WS2812 status LEDs (both show the same color).
constexpr int      LED_PIN_A       = 38;
constexpr int      LED_PIN_B       = 39;
constexpr uint8_t  LED_BRIGHTNESS  = 10;
// Color order copied from the board bring-up sketch (HG-ESP32S3-LTE). Plain WS2812 is
// usually NEO_GRB; switch here if "blue" shows up as another color.
#define LED_COLOR_ORDER NEO_BRG

// USB CLI
constexpr size_t   CLI_LINE_MAX    = 192;

#endif
