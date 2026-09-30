#ifndef DEF_H_
#define DEF_H_



#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#include "err_code.h"


#define _DEF_CH1              0
#define _DEF_CH2              1
#define _DEF_CH3              2
#define _DEF_CH4              3

#define _DEF_LED1             0

#define _DEF_UART1            0

#define _DEF_BUTTON1          0
#define _DEF_BUTTON2          1
#define _DEF_BUTTON3          2
#define _DEF_BUTTON4          3


#define _DEF_I2C1             0



#define _DEF_LOW              0
#define _DEF_HIGH             1

#define _DEF_INPUT            (1<<0)
#define _DEF_OUTPUT           (1<<1)
#define _DEF_PULLUP           (1<<2)
#define _DEF_PULLDOWN         (1<<3)
#define _DEF_IT_BOTH          (1<<4)
#define _DEF_INPUT_PULLUP     (_DEF_INPUT  | _DEF_PULLUP)
#define _DEF_INPUT_PULLDOWN   (_DEF_INPUT  | _DEF_PULLDOWN)
#define _DEF_OUTPUT_PULLUP    (_DEF_OUTPUT | _DEF_PULLUP)
#define _DEF_OUTPUT_PULLDOWN  (_DEF_OUTPUT | _DEF_PULLDOWN)
#define _DEF_INPUT_IT_BOTH    (_DEF_INPUT  | _DEF_PULLUP | _DEF_IT_BOTH)






#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))

#ifndef cmax
#define cmax(a,b) (((a) > (b)) ? (a) : (b))
#define cmin(a,b) (((a) < (b)) ? (a) : (b))
#endif


#ifndef cmap
#define cmap(value, in_min, in_max, out_min, out_max) ((value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min)
#endif


#define MAGIC_NUMBER              0x5555AAAA
#define FLASH_MAGIC_NUMBER        0x5555AAAA

#define VERSION_MAGIC_NUMBER      0x56455220    // "VER "
#define TAG_MAGIC_NUMBER          0x54414720    // "TAG "

typedef union
{
  uint8_t  u8Data[4];
  uint16_t u16Data[2];
  uint32_t u32Data;

  int8_t   s8Data[4];
  int16_t  s16Data[2];
  int32_t  s32Data;

  uint8_t  u8D;
  uint16_t u16D;
  uint32_t u32D;

  int8_t   s8D;
  int16_t  s16D;
  int32_t  s32D;
} data_t;

typedef struct 
{
  uint32_t magic_number;
  char     version_str[32];
  char     name_str[32];
  uint32_t firm_addr;
} firm_ver_t;

typedef struct 
{
  uint32_t magic_number;
  uint32_t fw_addr;
  uint32_t fw_size;
  uint32_t fw_crc;

  uint32_t tag_crc;
} firm_tag_t;

#endif