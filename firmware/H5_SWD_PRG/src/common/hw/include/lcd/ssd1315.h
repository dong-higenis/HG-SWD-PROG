/*
 * ssd1315.h
 *
 *  Created on: 2020. 12. 30.
 *      Author: baram
 */

#ifndef SRC_COMMON_HW_INCLUDE_LCD_SSD1315_H_
#define SRC_COMMON_HW_INCLUDE_LCD_SSD1315_H_

#include "hw_def.h"


#ifdef _USE_HW_SSD1315

#include "lcd.h"


bool ssd1315Init(void);
bool ssd1315InitDriver(lcd_driver_t *p_driver);

#endif


#endif /* SRC_COMMON_HW_INCLUDE_LCD_SSD1315_H_ */
