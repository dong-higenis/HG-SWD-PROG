#include "led.h"
#include "swtimer.h"


#ifdef _USE_HW_LED



const typedef struct 
{
  GPIO_TypeDef *port;
  uint16_t      pin;
  GPIO_PinState on_state;
  GPIO_PinState off_state;
} led_tbl_t;


static led_tbl_t led_tbl[LED_MAX_CH] = 
{
  {GPIOC, GPIO_PIN_13,  GPIO_PIN_RESET, GPIO_PIN_SET},
};

static volatile uint32_t activity_time;
static volatile bool activity_pending;
static bool activity_lit;
static uint32_t activity_edge;

static void activityTimer(void *arg)
{
  (void)arg;
  uint32_t now = millis();
  if (!activity_pending || (uint32_t)(now - activity_time) >= 150U)
  {
    activity_pending = false;
    if (activity_lit) ledOff(_DEF_LED1);
    activity_lit = false;
    return;
  }
  if ((uint32_t)(now - activity_edge) >= 60U)
  {
    activity_lit = !activity_lit;
    activity_edge = now;
    if (activity_lit) ledOn(_DEF_LED1);
    else ledOff(_DEF_LED1);
  }
}

void ledActivityPulse(void)
{
  activity_time = millis();
  activity_pending = true;
}

void ledActivityStop(void)
{
  uint32_t mask = __get_PRIMASK();
  __disable_irq();
  activity_pending = false;
  activity_lit = false;
  ledOff(_DEF_LED1);
  __set_PRIMASK(mask);
}



bool ledInit(void)
{
  GPIO_InitTypeDef   GPIO_InitStructure;


  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitStructure.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStructure.Pull  = GPIO_NOPULL;
  GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_LOW;

  for (int i=0; i<LED_MAX_CH; i++)
  {
    GPIO_InitStructure.Pin = led_tbl[i].pin;
    HAL_GPIO_Init(led_tbl[i].port, &GPIO_InitStructure);

    ledOff(i);
  }

  swtimer_handle_t timer = swtimerGetHandle();
  if (timer < 0) return false;
  swtimerSet(timer, 10, LOOP_TIME, activityTimer, NULL);
  swtimerStart(timer);
  return true;
}

void ledOn(uint8_t ch)
{
  if (ch >= LED_MAX_CH) return;

  HAL_GPIO_WritePin(led_tbl[ch].port, led_tbl[ch].pin, led_tbl[ch].on_state);
}

void ledOff(uint8_t ch)
{
  if (ch >= LED_MAX_CH) return;

  HAL_GPIO_WritePin(led_tbl[ch].port, led_tbl[ch].pin, led_tbl[ch].off_state);
}

void ledToggle(uint8_t ch)
{
  if (ch >= LED_MAX_CH) return;

  HAL_GPIO_TogglePin(led_tbl[ch].port, led_tbl[ch].pin);
}
#endif
