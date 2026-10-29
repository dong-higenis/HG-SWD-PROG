#include "status_led.h"

#include <Adafruit_NeoPixel.h>

#include "config.h"
#include "net.h"
#include "net_wifi.h"

namespace
{
constexpr uint32_t LED_UPDATE_MS = 100;

Adafruit_NeoPixel leds[] = {
  Adafruit_NeoPixel(1, LED_PIN_A, LED_COLOR_ORDER + NEO_KHZ800),
  Adafruit_NeoPixel(1, LED_PIN_B, LED_COLOR_ORDER + NEO_KHZ800),
};

uint32_t shown_color = 0xFFFFFFFF;  // forces the first update
uint32_t last_ms     = 0;

void show(uint32_t color)
{
  if (color == shown_color)
  {
    return;
  }
  shown_color = color;
  for (Adafruit_NeoPixel &led : leds)
  {
    led.setPixelColor(0, color);
    led.show();
  }
}
}  // namespace

void statusLedInit(void)
{
  for (Adafruit_NeoPixel &led : leds)
  {
    led.begin();
    led.setBrightness(LED_BRIGHTNESS);
  }
  show(0);
}

void statusLedTask(void)
{
  if (millis() - last_ms < LED_UPDATE_MS)
  {
    return;
  }
  last_ms = millis();

  NetStatusInfo st;
  netGetStatus(&st);

  uint32_t color = 0;
  switch (st.state)
  {
    case STATE_CONNECTING: color = Adafruit_NeoPixel::Color(0, 0, 255); break;
    case STATE_CONNECTED:  color = Adafruit_NeoPixel::Color(0, 255, 0); break;
    case STATE_FAILED:     color = Adafruit_NeoPixel::Color(255, 0, 0); break;
    default:
      if (st.mode == MODE_WIFI && wifiScanState() == APSCAN_BUSY)
      {
        color = Adafruit_NeoPixel::Color(0, 0, 255);
      }
      break;
  }
  show(color);
}
