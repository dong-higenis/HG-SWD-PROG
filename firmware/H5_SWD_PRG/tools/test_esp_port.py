"""Run the actual software UART ISR/queue logic against simulated GPIO/timer registers."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc',default=shutil.which('gcc'))
a=p.parse_args()
if not a.cc: p.error('Native GCC required')
work=root/'build-swd/host-tests/esp-port';work.mkdir(parents=True,exist_ok=True)
env=dict(os.environ);env['PATH']=str(Path(a.cc).parent)+os.pathsep+env.get('PATH','')
(work/'hw_def.h').write_text(r"""
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
typedef struct {uint32_t IDR;} GPIO_TypeDef;
typedef struct {uint32_t DIER,FPR1,IMR1,CCR1,CNT,SR,CR1,PSC,ARR,EGR;} Reg;
extern GPIO_TypeDef gb,gc;extern Reg tm,ex,rc;
#define GPIOB (&gb)
#define GPIOC (&gc)
#define TIM2 (&tm)
#define EXTI (&ex)
#define RCC (&rc)
#define CFGR1 CR1
#define UART7 ((void *)7)
#define GPIO_PIN_0 1
#define GPIO_PIN_1 2
#define GPIO_PIN_3 8
#define GPIO_PIN_4 16
#define GPIO_PIN_RESET 0
#define GPIO_PIN_SET 1
#define GPIO_MODE_INPUT 0
#define GPIO_MODE_OUTPUT_OD 1
#define GPIO_MODE_AF_PP 2
#define GPIO_MODE_IT_FALLING 3
#define GPIO_PULLUP 1
#define GPIO_NOPULL 0
#define GPIO_SPEED_FREQ_VERY_HIGH 3
#define GPIO_AF11_UART7 11
#define TIM_DIER_CC1IE 2
#define TIM_SR_CC1IF 2
#define TIM_EGR_UG 1
#define TIM_CR1_CEN 1
#define RCC_CFGR1_TIMPRE 1
#define RCC_PERIPHCLK_UART7 1
#define RCC_UART7CLKSOURCE_PCLK1 0
#define EXTI1_IRQn 1
#define TIM2_IRQn 2
#define UART_WORDLENGTH_8B 0
#define UART_STOPBITS_1 0
#define UART_PARITY_NONE 0
#define UART_MODE_TX 1
#define UART_HWCONTROL_NONE 0
#define UART_OVERSAMPLING_16 0
#define UART_PRESCALER_DIV1 0
#define __HAL_RCC_UART7_CLK_DISABLE() ((void)0)
#define __HAL_RCC_TIM2_CLK_DISABLE() ((void)0)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
#define __HAL_RCC_UART7_CLK_ENABLE() ((void)0)
#define __HAL_RCC_TIM2_CLK_ENABLE() ((void)0)
#define __HAL_RCC_TIM2_FORCE_RESET() ((void)0)
#define __HAL_RCC_TIM2_RELEASE_RESET() ((void)0)
#define __DMB() ((void)0)
#define __get_PRIMASK() 0
#define __disable_irq() ((void)0)
#define __set_PRIMASK(x) ((void)(x))
typedef enum {HAL_OK,HAL_ERROR,HAL_TIMEOUT} HAL_StatusTypeDef;
typedef struct {uint32_t Pin,Mode,Pull,Speed,Alternate;} GPIO_InitTypeDef;
typedef struct {uint32_t PeriphClockSelection,Uart7ClockSelection;} RCC_PeriphCLKInitTypeDef;
typedef struct {void *Instance;struct {uint32_t BaudRate,WordLength,StopBits,Parity,Mode,HwFlowCtl,OverSampling,ClockPrescaler;} Init;} UART_HandleTypeDef;
uint32_t millis(void);
void delay(uint32_t ms);
void HAL_NVIC_DisableIRQ(int n);
void HAL_NVIC_EnableIRQ(int n);
void HAL_NVIC_ClearPendingIRQ(int n);
void HAL_NVIC_SetPriority(int n,int p,int sub);
HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *u);
HAL_StatusTypeDef HAL_UART_DeInit(UART_HandleTypeDef *u);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *u,const uint8_t *d,uint16_t n,uint32_t t);
HAL_StatusTypeDef HAL_RCCEx_PeriphCLKConfig(RCC_PeriphCLKInitTypeDef *c);
uint32_t HAL_RCC_GetHCLKFreq(void);
uint32_t HAL_RCC_GetPCLK1Freq(void);
void HAL_GPIO_Init(GPIO_TypeDef *p,GPIO_InitTypeDef *c);
void HAL_GPIO_WritePin(GPIO_TypeDef *p,uint32_t pins,uint32_t state);
""",encoding='ascii')
for name,body in [('cli.h','void cliPrintf(const char *,...);'),('led.h','void ledActivityPulse(void);'),('swd.h','void swdRelease(void);')]:
    (work/name).write_text('#pragma once\n'+body+'\n')
source=work/'test_port.c'
source.write_text(r"""
#include "hw_def.h"
#include "esp_port.c"
#include <assert.h>
Reg tm,ex,rc;
GPIO_TypeDef gb,gc;
static uint32_t tick;
static int released;
static bool init_error;
uint32_t millis(void) { return tick; }
void delay(uint32_t ms) { tick+=ms; }
void cliPrintf(const char *f,...) { (void)f; }
void ledActivityPulse(void) {}
void swdRelease(void) { released++; }
void HAL_NVIC_DisableIRQ(int n) {(void)n;}
void HAL_NVIC_EnableIRQ(int n) {(void)n;}
void HAL_NVIC_ClearPendingIRQ(int n) {(void)n;}
void HAL_NVIC_SetPriority(int n,int p,int sub) {(void)n;(void)sub;assert(p<5);}
HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *u) {assert(u->Init.BaudRate>=115200 && u->Init.BaudRate<=921600 && u->Init.Mode==UART_MODE_TX);return init_error?HAL_ERROR:HAL_OK;}
HAL_StatusTypeDef HAL_UART_DeInit(UART_HandleTypeDef *u) {(void)u;return HAL_OK;}
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *u,const uint8_t *d,uint16_t n,uint32_t t) {
  (void)u;(void)d;(void)n;(void)t;return HAL_OK;
}
HAL_StatusTypeDef HAL_RCCEx_PeriphCLKConfig(RCC_PeriphCLKInitTypeDef *c) {(void)c;return HAL_OK;}
uint32_t HAL_RCC_GetHCLKFreq(void) {return 250000000;}
uint32_t HAL_RCC_GetPCLK1Freq(void) {return 250000000;}
void HAL_GPIO_Init(GPIO_TypeDef *p,GPIO_InitTypeDef *c) {if(p==GPIOC && c->Mode==GPIO_MODE_IT_FALLING)ex.IMR1|=RX_PIN;}
void HAL_GPIO_WritePin(GPIO_TypeDef *p,uint32_t pins,uint32_t state) {(void)p;(void)pins;(void)state;}
static void sample(bool high) {
  gc.IDR=high?RX_PIN:0;tm.CNT=tm.CCR1;tm.SR=TIM_SR_CC1IF;TIM2_IRQHandler();
}
static void start(void) {
  gc.IDR=0;tm.CNT+=bit_ticks/2;ex.FPR1=RX_PIN;EXTI1_IRQHandler();assert(tm.DIER==TIM_DIER_CC1IE);
}
static void byte(uint8_t b,bool stop) {
  start();sample(false);for(unsigned i=0;i<8;i++)sample((b>>i)&1);sample(stop);
  assert(!tm.DIER && (ex.IMR1&RX_PIN));
}
int main(void) {
  esp_loader_port_t *p=espPortGet();assert(p->ops->init(p)==ESP_LOADER_SUCCESS);
  assert(bit_ticks==2170);
  /* All byte values, back-to-back frames, around the timer wrap. */
  tm.CNT=0xfffffff0;
  for(unsigned i=0;i<256;i++)byte((uint8_t)i,true);
  uint8_t data[256];assert(p->ops->read(p,data,256,1)==ESP_LOADER_SUCCESS);
  for(unsigned i=0;i<256;i++)assert(data[i]==i);
  assert(!stats.framing&&!stats.late&&!stats.overflow);
  /* A short pulse is rejected as a false start. */
  start();sample(true);assert(rx_in==rx_out && !tm.DIER);
  byte(0xA5,false);assert(stats.framing==1);
  assert(p->ops->read(p,data,1,1)==ESP_LOADER_ERROR_INVALID_RESPONSE);
  espPortFlush();start();tm.CNT=tm.CCR1+bit_ticks/2;tm.SR=TIM_SR_CC1IF;TIM2_IRQHandler();assert(stats.late==1);
  espPortFlush();for(unsigned i=0;i<RX_CAPACITY+1;i++)byte((uint8_t)i,true);assert(stats.overflow==1);
  espPortFlush();assert(p->ops->read(p,data,1,2)==ESP_LOADER_ERROR_TIMEOUT);
  p->ops->start_timer(p,20);assert(p->ops->remaining_time(p)==20);delay(21);assert(!p->ops->remaining_time(p));
  const uint32_t rates[]={921600,460800,230400,115200};
  for(unsigned r=0;r<4;r++) {
    assert(p->ops->change_transmission_rate(p,rates[r])==ESP_LOADER_SUCCESS);
    assert(stats.baud==rates[r]&&bit_ticks==(250000000+rates[r]/2)/rates[r]);
    tm.CNT=0xfffffff0;
    for(unsigned i=0;i<256;i++)byte((uint8_t)i,true);
    assert(p->ops->read(p,data,256,1)==ESP_LOADER_SUCCESS);
    for(unsigned i=0;i<256;i++)assert(data[i]==i);
  }
  assert(p->ops->change_transmission_rate(p,12345)==ESP_LOADER_ERROR_INVALID_PARAM);
  init_error=true;assert(p->ops->change_transmission_rate(p,460800)==ESP_LOADER_ERROR_FAIL);
  assert(!(ex.IMR1&RX_PIN)&&!tm.DIER);init_error=false;
  espPortClose();assert(!active&&!tm.DIER&&!tm.CR1&&!(ex.IMR1&RX_PIN)&&released>=2);
  puts("PASS: actual EXTI1/TIM2 RX ISR: all 256 bytes, timer wrap, false start, stop error, latency, overflow, timeout, release.");
}
""",encoding='ascii')
exe=work/'test_port.exe'
subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror','-I'+str(work),
                '-I'+str(root/'src/hw/driver/esp'),'-I'+str(root/'src/lib/esp-serial-flasher/include'),
                str(source),'-o',str(exe)],check=True,env=env)
subprocess.run([str(exe)],check=True,env=env)
