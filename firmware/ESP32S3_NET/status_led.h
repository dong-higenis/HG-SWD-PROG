#ifndef STATUS_LED_H_
#define STATUS_LED_H_

// WS2812 status: off = network off/idle, blue = connecting or scanning,
// green = connected, red = failed.
void statusLedInit(void);
void statusLedTask(void);

#endif
