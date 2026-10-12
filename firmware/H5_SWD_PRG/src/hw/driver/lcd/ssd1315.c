/*
 * ssd1315.c
 *
 *  Created on: 2020. 12. 30.
 *      Author: baram
 */


#include "lcd/ssd1315.h"
#include "i2c.h"


#ifdef _USE_HW_SSD1315

#define SSD1315_WIDTH  128
#define SSD1315_HEIGHT 64


static uint8_t i2c_ch              = _DEF_I2C1;
static uint8_t i2c_dev             = 0x78 >> 1; // 0x3C
static void (*frameCallBack)(void) = NULL;


static bool     ssd1315Reset(void);
static void     ssd1315SetWindow(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
static uint16_t ssd1315GetWidth(void);
static uint16_t ssd1315GetHeight(void);
static bool     ssd1315SendBuffer(uint8_t *p_data, uint32_t length, uint32_t timeout_ms);
static bool     ssd1315SetCallBack(void (*p_func)(void));
static void     ssd1315Fill(uint16_t color);
static bool     ssd1315UpdateDraw(void);
static void     ssd1315DrawPixel(uint8_t x, uint8_t y, uint16_t color);


static uint8_t ssd1315_buffer[SSD1315_WIDTH * SSD1315_HEIGHT / 8];
static uint8_t ssd1315_shown[SSD1315_WIDTH * SSD1315_HEIGHT / 8]; /* What the panel holds after the last good frame. */
static bool    ssd1315_shown_valid = false;

bool ssd1315Init(void)
{
  bool ret;

  ret = ssd1315Reset();

  return ret;
}

bool ssd1315InitDriver(lcd_driver_t *p_driver)
{
  p_driver->init        = ssd1315Init;
  p_driver->reset       = ssd1315Reset;
  p_driver->setWindow   = ssd1315SetWindow;
  p_driver->getWidth    = ssd1315GetWidth;
  p_driver->getHeight   = ssd1315GetHeight;
  p_driver->setCallBack = ssd1315SetCallBack;
  p_driver->sendBuffer  = ssd1315SendBuffer;
  return true;
}

bool ssd1315WriteCmd(uint8_t cmd_data)
{
  return i2cWriteByte(i2c_ch, i2c_dev, 0x00, cmd_data, 10);
}

bool ssd1315Reset(void)
{
  bool ret;

  ssd1315_shown_valid = false;
  delay(25);
  ret = i2cBegin(i2c_ch, 400);

  if (ret != true)
  {
    return false;
  }

  if (i2cIsDeviceReady(i2c_ch, i2c_dev) != true)
  {
    return false;
  }

  /* SSD1315, 128x64, internal charge pump, page addressing (20h,02h).
   * The four-pin module provides its own power-on reset. */
  const uint8_t commands[] = {0xAE, 0x20, 0x02, 0xB0, 0x00, 0x10, 0x40, 0xA1, 0xC8,
                              0x81, 0x7F, 0xA6, 0xA8, 0x3F, 0xA4, 0xD3, 0x00, 0xD5,
                              0x80, 0xD9, 0x22, 0xDA, 0x12, 0xDB, 0x20, 0xAD, HW_SSD1315_IREF,
                              0x8D, 0x14, 0x2E};
  for (uint32_t i = 0; i < sizeof(commands); i++)
  {
    if (!ssd1315WriteCmd(commands[i]))
    {
      return false;
    }
  }
  ssd1315Fill(black);
  if (!ssd1315UpdateDraw())
  {
    return false;
  }
  return ssd1315WriteCmd(0xAF);
}

void ssd1315SetWindow(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
}

uint16_t ssd1315GetWidth(void)
{
  return LCD_WIDTH;
}

uint16_t ssd1315GetHeight(void)
{
  return LCD_HEIGHT;
}

bool ssd1315SendBuffer(uint8_t *p_data, uint32_t length, uint32_t timeout_ms)
{
  uint16_t *p_buf = (uint16_t *)p_data;


  for (int y = 0; y < SSD1315_HEIGHT; y++)
  {
    for (int x = 0; x < SSD1315_WIDTH; x++)
    {
      ssd1315DrawPixel(x, y, p_buf[y * LCD_WIDTH + x]);
    }
  }

  bool ret = ssd1315UpdateDraw();

  if (frameCallBack != NULL)
  {
    frameCallBack();
  }
  return ret;
}

bool ssd1315SetCallBack(void (*p_func)(void))
{
  frameCallBack = p_func;

  return true;
}

void ssd1315Fill(uint16_t color)
{
  uint32_t i;

  for (i = 0; i < sizeof(ssd1315_buffer); i++)
  {
    ssd1315_buffer[i] = (color > 0) ? 0xFF : 0x00;
  }
}

bool ssd1315UpdateDraw(void)
{
  uint8_t i;

  /* Unchanged pages are skipped: an animation that moves one strip costs one strip of I2C time. */
  for (i = 0; i < 8; i++)
  {
    uint8_t *page = &ssd1315_buffer[SSD1315_WIDTH * i];
    if (ssd1315_shown_valid && memcmp(page, &ssd1315_shown[SSD1315_WIDTH * i], SSD1315_WIDTH) == 0)
    {
      continue;
    }

    if (!ssd1315WriteCmd(0xB0 + i) || !ssd1315WriteCmd(0x00) || !ssd1315WriteCmd(0x10) ||
        i2cWriteBytes(i2c_ch, i2c_dev, 0x40, page, SSD1315_WIDTH, 100) == false)
    {
      ssd1315_shown_valid = false; /* Panel contents unknown: resend everything next time. */
      return false;
    }
    memcpy(&ssd1315_shown[SSD1315_WIDTH * i], page, SSD1315_WIDTH);
  }
  ssd1315_shown_valid = true;

  return true;
}

void ssd1315DrawPixel(uint8_t x, uint8_t y, uint16_t color)
{
  if (x >= SSD1315_WIDTH || y >= SSD1315_HEIGHT)
  {
    return;
  }


  if (color > 0)
  {
    ssd1315_buffer[x + (y / 8) * SSD1315_WIDTH] |= 1 << (y % 8);
  }
  else
  {
    ssd1315_buffer[x + (y / 8) * SSD1315_WIDTH] &= ~(1 << (y % 8));
  }
}

#endif
