
#pragma once
#include "hw_def.h"
#include <stdio.h>
typedef struct { const char *name; int priority; bool (*init)(void); } module_t;
#define MODULE_DEF(n) static module_t module_##n =
#define MODULE_PRI_HIGH 1
typedef struct { unsigned argc; bool (*isStr)(uint8_t,const char *); char *(*getStr)(uint8_t); } cli_args_t;
enum { BTN_UP, BTN_DN, BTN_SEL, BTN_M };
enum { black=0,white=65535,LCD_FONT_07x10=0,LCD_FONT_11x18=1,_DEF_LED1=0 };
void cliPrintf(const char *,...);
bool cliAdd(const char *,void (*)(cli_args_t *));
bool sdIsDetected(void);
bool buttonGetPressed(uint8_t);
uint32_t buttonGetData(void);
void delay(uint32_t);
void ledToggle(int);
void ledActivityPulse(void);
void ledActivityStop(void);
bool lcdIsInit(void);
void lcdSetFont(int);
void lcdClearBuffer(uint32_t);
void lcdPrintf(int,int,uint16_t,const char *,...);
void lcdDrawFillRect(int,int,int,int,uint16_t);
void lcdDrawRect(int,int,int,int,uint16_t);
void lcdUpdateDraw(void);
