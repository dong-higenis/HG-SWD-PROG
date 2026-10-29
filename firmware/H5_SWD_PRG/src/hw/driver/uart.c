#include "uart.h"
#include "qbuffer.h"
#include "cli.h"

#define UART_RX_BUF_LENGTH 2048

/* One entry per channel: RX runs as a circular GPDMA transfer into rx_buf, TX is blocking. */
typedef struct
{
  USART_TypeDef      *instance;
  DMA_Channel_TypeDef *dma_channel;
  uint32_t            dma_request;
  const char         *pins;
  UART_HandleTypeDef  huart;
  DMA_HandleTypeDef   hdma;
  DMA_NodeTypeDef     node;
  DMA_QListTypeDef    list;
  qbuffer_t           rx_queue;
  bool                is_open;
  uint32_t            baud_rate;
  uint32_t            rx_count;
  uint32_t            tx_count;
  SemaphoreHandle_t   tx_mutex;
} uart_tbl_t;

static uint8_t rx_buf[UART_MAX_CH][UART_RX_BUF_LENGTH] __attribute__((section(".non_cache"), aligned(32)));
static uart_tbl_t uart_tbl[UART_MAX_CH] = {
  [_DEF_UART1] = {.instance    = USART1,
                  .dma_channel = GPDMA1_Channel0,
                  .dma_request = GPDMA1_REQUEST_USART1_RX,
                  .pins        = "PB15=TX PB14=RX (swap)"},
  [_DEF_UART2] = {.instance    = USART6,
                  .dma_channel = GPDMA1_Channel1,
                  .dma_request = GPDMA1_REQUEST_USART6_RX,
                  .pins        = "PC6=TX PC7=RX (ESP link)"},
};
static bool is_init;

/* Read with `p uart_debug` even when the physical serial link is silent (debug channel only). */
volatile struct
{
  uint32_t          open_attempts;
  uint32_t          stage; /* 0=not opened, 1=HAL init, 2=RX DMA, 3=open */
  HAL_StatusTypeDef init_status;
  HAL_StatusTypeDef rx_status;
  HAL_StatusTypeDef tx_status;
  uint32_t          hal_error;
  uint32_t          tx_attempts;
  uint32_t          tx_failures;
} uart_debug;

static void cliUart(cli_args_t *args);

bool uartInit(void)
{
  for (unsigned ch = 0; ch < UART_MAX_CH; ch++)
  {
    if (!uart_tbl[ch].tx_mutex)
    {
      uart_tbl[ch].tx_mutex = xSemaphoreCreateMutex();
    }
    if (!uart_tbl[ch].tx_mutex)
    {
      return false;
    }
  }
  is_init = true;
  cliAdd("uart", cliUart);
  return true;
}

bool uartDeInit(void)
{
  for (unsigned ch = 0; ch < UART_MAX_CH; ch++)
  {
    uartClose(ch);
  }
  is_init = false;
  return true;
}

bool uartIsInit(void)
{
  return is_init;
}

bool uartOpen(uint8_t ch, uint32_t baud)
{
  if (!is_init || ch >= UART_MAX_CH || baud == 0)
  {
    return false;
  }
  uart_tbl_t *p = &uart_tbl[ch];
  if (p->is_open && p->baud_rate == baud)
  {
    return true;
  }
  bool debug = ch == HW_UART_CH_DEBUG;
  if (debug)
  {
    uart_debug.open_attempts++;
  }
  uartClose(ch);
  memset(&p->huart, 0, sizeof(p->huart));
  memset(&p->hdma, 0, sizeof(p->hdma));
  memset(&p->node, 0, sizeof(p->node));
  memset(&p->list, 0, sizeof(p->list));
  __HAL_RCC_GPDMA1_CLK_ENABLE();
  p->huart.Instance            = p->instance;
  p->huart.Init.BaudRate       = baud;
  p->huart.Init.WordLength     = UART_WORDLENGTH_8B;
  p->huart.Init.StopBits       = UART_STOPBITS_1;
  p->huart.Init.Parity         = UART_PARITY_NONE;
  p->huart.Init.Mode           = UART_MODE_TX_RX;
  p->huart.Init.HwFlowCtl      = UART_HWCONTROL_NONE;
  p->huart.Init.OverSampling   = UART_OVERSAMPLING_16;
  p->huart.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  if (p->instance == USART1)
  {
    p->huart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_SWAP_INIT;
    p->huart.AdvancedInit.Swap           = UART_ADVFEATURE_SWAP_ENABLE;
  }
  qbufferCreate(&p->rx_queue, rx_buf[ch], UART_RX_BUF_LENGTH);
  HAL_StatusTypeDef status = HAL_UART_Init(&p->huart);
  if (debug)
  {
    uart_debug.stage       = 1;
    uart_debug.init_status = status;
    uart_debug.hal_error   = p->huart.ErrorCode;
  }
  if (status != HAL_OK)
  {
    return false;
  }
  status = HAL_UART_Receive_DMA(&p->huart, rx_buf[ch], UART_RX_BUF_LENGTH);
  if (debug)
  {
    uart_debug.stage     = 2;
    uart_debug.rx_status = status;
    uart_debug.hal_error = p->huart.ErrorCode;
  }
  if (status != HAL_OK)
  {
    HAL_UART_DeInit(&p->huart);
    return false;
  }
  p->baud_rate = baud;
  p->is_open   = true;
  if (debug)
  {
    uart_debug.stage = 3;
  }
  return true;
}

bool uartClose(uint8_t ch)
{
  if (ch >= UART_MAX_CH)
  {
    return false;
  }
  uart_tbl_t *p = &uart_tbl[ch];
  if (p->is_open)
  {
    HAL_UART_DMAStop(&p->huart);
    HAL_UART_DeInit(&p->huart);
  }
  p->is_open = false;
  if (ch == HW_UART_CH_DEBUG)
  {
    uart_debug.stage = 0;
  }
  return true;
}

uint32_t uartAvailable(uint8_t ch)
{
  if (ch >= UART_MAX_CH || !uart_tbl[ch].is_open)
  {
    return 0;
  }
  uart_tbl_t *p = &uart_tbl[ch];
  p->rx_queue.in = (UART_RX_BUF_LENGTH - __HAL_DMA_GET_COUNTER(p->huart.hdmarx)) % UART_RX_BUF_LENGTH;
  return qbufferAvailable(&p->rx_queue);
}

bool uartFlush(uint8_t ch)
{
  if (ch >= UART_MAX_CH || !uart_tbl[ch].is_open)
  {
    return false;
  }
  uartAvailable(ch);
  uart_tbl[ch].rx_queue.out = uart_tbl[ch].rx_queue.in;
  return true;
}

uint8_t uartRead(uint8_t ch)
{
  uint8_t data = 0;
  if (uartAvailable(ch) && qbufferRead(&uart_tbl[ch].rx_queue, &data, 1))
  {
    uart_tbl[ch].rx_count++;
  }
  return data;
}

uint32_t uartWrite(uint8_t ch, uint8_t *data, uint32_t length)
{
  if (ch >= UART_MAX_CH || !uart_tbl[ch].is_open || !data || length > UINT16_MAX)
  {
    return 0;
  }
  uart_tbl_t *p = &uart_tbl[ch];
  if (__get_IPSR() != 0 || !p->tx_mutex)
  {
    return 0;
  }
  TickType_t wait = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING ? pdMS_TO_TICKS(1100) : 0;
  if (xSemaphoreTake(p->tx_mutex, wait) != pdTRUE)
  {
    return 0;
  }
  bool              debug  = ch == HW_UART_CH_DEBUG;
  HAL_StatusTypeDef status = HAL_UART_Transmit(&p->huart, data, length, 1000);
  if (debug)
  {
    uart_debug.tx_attempts++;
    uart_debug.tx_status = status;
    uart_debug.hal_error = p->huart.ErrorCode;
    uart_debug.tx_failures += status != HAL_OK;
  }
  if (status == HAL_OK)
  {
    p->tx_count += length;
  }
  xSemaphoreGive(p->tx_mutex);
  return status == HAL_OK ? length : 0;
}

uint32_t uartPrintf(uint8_t ch, const char *fmt, ...)
{
  char    buf[256];
  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (len <= 0)
  {
    return 0;
  }
  if (len >= (int)sizeof(buf))
  {
    len = sizeof(buf) - 1;
  }
  return uartWrite(ch, (uint8_t *)buf, len);
}

uint32_t uartGetBaud(uint8_t ch)
{
  return ch < UART_MAX_CH ? uart_tbl[ch].baud_rate : 0;
}

uint32_t uartGetRxCnt(uint8_t ch)
{
  return ch < UART_MAX_CH ? uart_tbl[ch].rx_count : 0;
}

uint32_t uartGetTxCnt(uint8_t ch)
{
  return ch < UART_MAX_CH ? uart_tbl[ch].tx_count : 0;
}

static uart_tbl_t *uartFind(USART_TypeDef *instance)
{
  for (unsigned ch = 0; ch < UART_MAX_CH; ch++)
  {
    if (uart_tbl[ch].instance == instance)
    {
      return &uart_tbl[ch];
    }
  }
  return NULL;
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *handle)
{
  if (!uartFind(handle->Instance))
  {
    return;
  }
  if (handle->hdmarx)
  {
    HAL_DMA_DeInit(handle->hdmarx);
  }
  if (handle->Instance == USART1)
  {
    __HAL_RCC_USART1_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_14 | GPIO_PIN_15);
  }
  else
  {
    __HAL_RCC_USART6_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_6 | GPIO_PIN_7);
  }
}

static void cliUart(cli_args_t *args)
{
  (void)args;
  for (unsigned ch = 0; ch < UART_MAX_CH; ch++)
  {
    const uart_tbl_t *p = &uart_tbl[ch];
    cliPrintf("UART%u: %s, %lu bps, open=%d, rx=%lu tx=%lu\n", ch + 1, p->pins, p->baud_rate, p->is_open,
              p->rx_count, p->tx_count);
  }
}

/* RX: one circular linked-list node per channel. */
static void uartInitRxDma(uart_tbl_t *p, UART_HandleTypeDef *uartHandle)
{
  DMA_NodeConfTypeDef NodeConfig = {0};

  NodeConfig.NodeType                         = DMA_GPDMA_LINEAR_NODE;
  NodeConfig.Init.Request                     = p->dma_request;
  NodeConfig.Init.BlkHWRequest                = DMA_BREQ_SINGLE_BURST;
  NodeConfig.Init.Direction                   = DMA_PERIPH_TO_MEMORY;
  NodeConfig.Init.SrcInc                      = DMA_SINC_FIXED;
  NodeConfig.Init.DestInc                     = DMA_DINC_INCREMENTED;
  NodeConfig.Init.SrcDataWidth                = DMA_SRC_DATAWIDTH_BYTE;
  NodeConfig.Init.DestDataWidth               = DMA_DEST_DATAWIDTH_BYTE;
  NodeConfig.Init.SrcBurstLength              = 1;
  NodeConfig.Init.DestBurstLength             = 1;
  NodeConfig.Init.TransferAllocatedPort       = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0;
  NodeConfig.Init.TransferEventMode           = DMA_TCEM_BLOCK_TRANSFER;
  NodeConfig.Init.Mode                        = DMA_NORMAL;
  NodeConfig.TriggerConfig.TriggerPolarity    = DMA_TRIG_POLARITY_MASKED;
  NodeConfig.DataHandlingConfig.DataExchange  = DMA_EXCHANGE_NONE;
  NodeConfig.DataHandlingConfig.DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED;
  if (HAL_DMAEx_List_BuildNode(&NodeConfig, &p->node) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_DMAEx_List_InsertNode(&p->list, NULL, &p->node) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_DMAEx_List_SetCircularMode(&p->list) != HAL_OK)
  {
    Error_Handler();
  }

  p->hdma.Instance                         = p->dma_channel;
  p->hdma.InitLinkedList.Priority          = DMA_LOW_PRIORITY_LOW_WEIGHT;
  p->hdma.InitLinkedList.LinkStepMode      = DMA_LSM_FULL_EXECUTION;
  p->hdma.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0;
  p->hdma.InitLinkedList.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
  p->hdma.InitLinkedList.LinkedListMode    = DMA_LINKEDLIST_CIRCULAR;
  if (HAL_DMAEx_List_Init(&p->hdma) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_DMAEx_List_LinkQ(&p->hdma, &p->list) != HAL_OK)
  {
    Error_Handler();
  }

  __HAL_LINKDMA(uartHandle, hdmarx, p->hdma);

  if (HAL_DMA_ConfigChannelAttributes(&p->hdma, DMA_CHANNEL_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_UART_MspInit(UART_HandleTypeDef *uartHandle)
{
  GPIO_InitTypeDef         GPIO_InitStruct     = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  uart_tbl_t              *p                   = uartFind(uartHandle->Instance);

  if (!p)
  {
    return;
  }
  if (uartHandle->Instance == USART1)
  {
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USART1;
    PeriphClkInitStruct.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }
    __HAL_RCC_USART1_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* AF4 with TX/RX swap: PB15 TX (U1_TXD), PB14 RX (U1_RXD). */
    GPIO_InitStruct.Pin       = GPIO_PIN_14 | GPIO_PIN_15;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_PULLUP;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF4_USART1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  }
  else
  {
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USART6;
    PeriphClkInitStruct.Usart6ClockSelection = RCC_USART6CLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }
    __HAL_RCC_USART6_CLK_ENABLE();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    /* AF7: PC6 TX -> ESP GPIO48, PC7 RX <- ESP GPIO45 (core header J4 pins 21/23). */
    GPIO_InitStruct.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_PULLUP;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART6;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
  }
  uartInitRxDma(p, uartHandle);
}
