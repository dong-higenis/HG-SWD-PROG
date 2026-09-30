#ifndef LED_H_
#define LED_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "hw_def.h"


#define LED_MAX_CH  HW_LED_MAX_CH


bool ledInit(void);
void ledOn(uint8_t ch);
void ledOff(uint8_t ch);
void ledToggle(uint8_t ch);
/* LED1 communication indicator: pulses extend a short activity window.
 * A 10ms software timer handles visible blinking independently of bulk I/O. */
void ledActivityPulse(void);
void ledActivityStop(void);

#ifdef __cplusplus
}
#endif

#endif
