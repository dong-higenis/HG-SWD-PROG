#ifndef HW_DEF_H_
#define HW_DEF_H_

#include "bsp.h"
#include "assert_def.h"

#define _DEF_FIRMWATRE_VERSION    "V261006R2"
#define _DEF_BOARD_NAME           "HG-SWD-PROG"

#define _USE_HW_ASSERT
#define _USE_HW_FAULT
#define _USE_HW_BUZZER
#define _USE_HW_SWD
#define _USE_HW_MICROS
#define _USE_HW_RTOS
// #define _NOT_USE_RTC 

#define _USE_HW_LED
#define      HW_LED_MAX_CH          1

#define _USE_HW_UART
#define      HW_UART_MAX_CH         2
#define      HW_UART_CH_DEBUG      _DEF_UART1
#define      HW_UART_CH_NET        _DEF_UART2   /* USART6 to the ESP32-S3 network module */

#define _USE_HW_CLI
#define      HW_CLI_CMD_LIST_MAX    32
#define      HW_CLI_CMD_NAME_MAX    16
#define      HW_CLI_LINE_HIS_MAX    8
#define      HW_CLI_LINE_BUF_MAX    64

#define _USE_HW_LOG
#define      HW_LOG_CH              HW_UART_CH_DEBUG
#define      HW_LOG_BOOT_BUF_MAX    2048
#define      HW_LOG_LIST_BUF_MAX    4096

#define _USE_HW_SWTIMER
#define      HW_SWTIMER_MAX_CH      16

#define _USE_HW_BUTTON
#define      HW_BUTTON_MAX_CH       BUTTON_PIN_MAX

#define _USE_HW_RESET
#define      HW_RESET_BOOT          1

#define _USE_HW_RTC
#define      HW_RTC_BOOT_MODE       RTC_BKP_DR3
#define      HW_RTC_RESET_BITS      RTC_BKP_DR4

#define _USE_HW_I2C
#define      HW_I2C_MAX_CH          1

#define _USE_HW_LCD
#define _USE_HW_SSD1315
#define      HW_SSD1315_IREF       0x30  // Internal IREF, 30 uA; 0x00 for external resistor
#define      HW_LCD_WIDTH           128
#define      HW_LCD_HEIGHT          64

#define _USE_HW_GPIO
#define      HW_GPIO_MAX_CH         GPIO_PIN_MAX

#define _USE_HW_SPI_FLASH
#define      HW_SPI_FLASH_ADDR      0x90000000

#define _USE_HW_SD
#define _USE_HW_FATFS
//-- CLI
//
#define _USE_CLI_HW_LOG             1
#define _USE_CLI_HW_ASSERT          1
#define _USE_CLI_HW_UART            1

//-- RTOS
//
#define _USE_HW_THREAD
#define      HW_THREAD_MAX_CNT                  2

#define _HW_DEF_THREAD_MAIN_PRI                 osPriorityNormal
#define _HW_DEF_THREAD_MAIN_STACK               (8*1024)

#define _HW_DEF_THREAD_CLI_PRI                  osPriorityNormal
#define _HW_DEF_THREAD_CLI_STACK                (32*1024)

typedef enum
{
  BTN_UP,
  BTN_DN,
  BTN_SEL,
  BTN_M,

  BUTTON_PIN_MAX
} ButtonPinName_t;

typedef enum
{
  SD_DETECT,
  TARGET_3V3,   /* PB5 high: 3V_OUT on the target connector */
  TARGET_5V,    /* PA9 high: 5V_OUT on the target connector */

  GPIO_PIN_MAX
} GpioPinName_t;

#endif
