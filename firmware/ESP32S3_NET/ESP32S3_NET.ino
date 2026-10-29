// ESP32-S3 network coprocessor for the HG-SWD-PROG (H5) offline programmer.
// The H5 drives this board over a framed UART (PROTOCOL.md) and uses it as a
// generic pipe: Wi-Fi or LTE connection + HTTPS GET streaming.
//
// Board: ESP32S3 Dev Module, USB CDC On Boot enabled for the debug console
// (see README.md for the exact FQBN).

#include "cli.h"
#include "config.h"
#include "link.h"
#include "log.h"
#include "net.h"
#include "status_led.h"

// TLS handshakes run inside the H5 request handler on the loop task.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

volatile bool g_log_verbose = false;

void setup()
{
  // Logging must never stall the H5 link: big TX buffer, and on USB CDC drop output
  // instead of waiting when no terminal is reading.
  Serial.setTxBufferSize(4096);
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);
#endif
  Serial.begin(115200);
  statusLedInit();
  netInit();
  linkInit();
  cliInit();
  LOGI("MAIN", "%s %s ready, H5 link %lu bps (RX GPIO%d, TX GPIO%d). Type 'help'.", NET_FW_NAME, NET_FW_VERSION,
       (unsigned long)H5_BAUD, H5_RX_PIN, H5_TX_PIN);
}

void loop()
{
  linkPoll();
  netTask();
  cliTask();
  statusLedTask();

  if (!linkActive())
  {
    delay(1);
  }
}
