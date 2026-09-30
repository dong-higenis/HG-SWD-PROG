#include "uart.h"
#include "qbuffer.h"
#include "cli.h"

#define UART_RX_BUF_LENGTH 1024
static UART_HandleTypeDef huart1;
static DMA_HandleTypeDef handle_GPDMA1_Channel0;
static DMA_NodeTypeDef Node_GPDMA1_Channel0;
static DMA_QListTypeDef List_GPDMA1_Channel0;
static uint8_t rx_buf[UART_RX_BUF_LENGTH] __attribute__((section(".non_cache"), aligned(32)));
static qbuffer_t rx_queue;
static bool is_init, is_open;
static uint32_t baud_rate, rx_count, tx_count;
static SemaphoreHandle_t tx_mutex;
/* Read with `p uart_debug` even when the physical serial link is silent. */
volatile struct
{
  uint32_t open_attempts;
  uint32_t stage; /* 0=not opened, 1=HAL init, 2=RX DMA, 3=open */
  HAL_StatusTypeDef init_status;
  HAL_StatusTypeDef rx_status;
  HAL_StatusTypeDef tx_status;
  uint32_t hal_error;
  uint32_t tx_attempts;
  uint32_t tx_failures;
} uart_debug;
static void cliUart(cli_args_t *args);

bool uartInit(void)
{
  if (!tx_mutex) tx_mutex = xSemaphoreCreateMutex();
  if (!tx_mutex) return false;
  is_init = true;
  cliAdd("uart", cliUart);
  return true;
}
bool uartDeInit(void) { uartClose(HW_UART_CH_DEBUG); is_init = false; return true; }
bool uartIsInit(void) { return is_init; }
bool uartOpen(uint8_t ch, uint32_t baud)
{
  if (!is_init || ch >= UART_MAX_CH || baud == 0) return false;
  if (is_open && baud_rate == baud) return true;
  uart_debug.open_attempts++;
  uartClose(ch);
  memset(&huart1, 0, sizeof(huart1));
  memset(&handle_GPDMA1_Channel0, 0, sizeof(handle_GPDMA1_Channel0));
  memset(&Node_GPDMA1_Channel0, 0, sizeof(Node_GPDMA1_Channel0));
  memset(&List_GPDMA1_Channel0, 0, sizeof(List_GPDMA1_Channel0));
  __HAL_RCC_GPDMA1_CLK_ENABLE();
  huart1.Instance = USART1;
  huart1.Init.BaudRate = baud;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_SWAP_INIT;
  huart1.AdvancedInit.Swap = UART_ADVFEATURE_SWAP_ENABLE;
  qbufferCreate(&rx_queue, rx_buf, sizeof(rx_buf));
  uart_debug.stage = 1;
  uart_debug.init_status = HAL_UART_Init(&huart1);
  uart_debug.hal_error = huart1.ErrorCode;
  if (uart_debug.init_status != HAL_OK) return false;
  uart_debug.stage = 2;
  uart_debug.rx_status = HAL_UART_Receive_DMA(&huart1, rx_buf, sizeof(rx_buf));
  uart_debug.hal_error = huart1.ErrorCode;
  if (uart_debug.rx_status != HAL_OK)
  {
    HAL_UART_DeInit(&huart1);
    return false;
  }
  baud_rate = baud;
  is_open = true;
  uart_debug.stage = 3;
  return true;
}
bool uartClose(uint8_t ch)
{
  if (ch >= UART_MAX_CH) return false;
  if (is_open) { HAL_UART_DMAStop(&huart1); HAL_UART_DeInit(&huart1); }
  is_open = false;
  uart_debug.stage = 0;
  return true;
}
uint32_t uartAvailable(uint8_t ch)
{
  if (ch >= UART_MAX_CH || !is_open) return 0;
  rx_queue.in = (UART_RX_BUF_LENGTH - __HAL_DMA_GET_COUNTER(huart1.hdmarx)) % UART_RX_BUF_LENGTH;
  return qbufferAvailable(&rx_queue);
}
bool uartFlush(uint8_t ch)
{
  if (ch >= UART_MAX_CH || !is_open) return false;
  uartAvailable(ch);
  rx_queue.out = rx_queue.in;
  return true;
}
uint8_t uartRead(uint8_t ch)
{
  uint8_t data = 0;
  if (uartAvailable(ch) && qbufferRead(&rx_queue, &data, 1)) rx_count++;
  return data;
}
uint32_t uartWrite(uint8_t ch, uint8_t *data, uint32_t length)
{
  if (ch >= UART_MAX_CH || !is_open || !data || length > UINT16_MAX) return 0;
  if (__get_IPSR() != 0 || !tx_mutex) return 0;
  TickType_t wait = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING ? pdMS_TO_TICKS(1100) : 0;
  if (xSemaphoreTake(tx_mutex, wait) != pdTRUE) return 0;
  uart_debug.tx_attempts++;
  uart_debug.tx_status = HAL_UART_Transmit(&huart1, data, length, 1000);
  uart_debug.hal_error = huart1.ErrorCode;
  if (uart_debug.tx_status != HAL_OK)
  {
    uart_debug.tx_failures++;
    xSemaphoreGive(tx_mutex);
    return 0;
  }
  tx_count += length;
  xSemaphoreGive(tx_mutex);
  return length;
}
uint32_t uartPrintf(uint8_t ch, const char *fmt, ...)
{
  char buf[256];
  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (len <= 0) return 0;
  if (len >= (int)sizeof(buf)) len = sizeof(buf)-1;
  return uartWrite(ch, (uint8_t *)buf, len);
}
uint32_t uartGetBaud(uint8_t ch) { return ch < UART_MAX_CH ? baud_rate : 0; }
uint32_t uartGetRxCnt(uint8_t ch) { return ch < UART_MAX_CH ? rx_count : 0; }
uint32_t uartGetTxCnt(uint8_t ch) { return ch < UART_MAX_CH ? tx_count : 0; }

void HAL_UART_MspDeInit(UART_HandleTypeDef *handle)
{
  if (handle->Instance != USART1) return;
  if (handle->hdmarx) HAL_DMA_DeInit(handle->hdmarx);
  __HAL_RCC_USART1_CLK_DISABLE();
  HAL_GPIO_DeInit(GPIOB, GPIO_PIN_14 | GPIO_PIN_15);
}
static void cliUart(cli_args_t *args)
{
  (void)args;
  cliPrintf("USART1: PB15=TX PB14=RX (swap), %lu bps, open=%d\n", baud_rate, is_open);
}
void HAL_UART_MspInit(UART_HandleTypeDef *uartHandle)
{
  GPIO_InitTypeDef         GPIO_InitStruct     = {0};
  DMA_NodeConfTypeDef      NodeConfig          = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  if (uartHandle->Instance == USART1)
  {
    /** Initializes the peripherals clock
     */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USART1;
    PeriphClkInitStruct.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    /* USART1 clock enable */
    __HAL_RCC_USART1_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* AF4 with TX/RX swap: PB15 TX (U1_TXD), PB14 RX (U1_RXD). */
    GPIO_InitStruct.Pin = GPIO_PIN_14 | GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF4_USART1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* USART1 DMA Init */
    /* GPDMA1_REQUEST_USART1_RX Init */
    NodeConfig.NodeType                         = DMA_GPDMA_LINEAR_NODE;
    NodeConfig.Init.Request                     = GPDMA1_REQUEST_USART1_RX;
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
    if (HAL_DMAEx_List_BuildNode(&NodeConfig, &Node_GPDMA1_Channel0) != HAL_OK)
    {
      Error_Handler();
    }

    if (HAL_DMAEx_List_InsertNode(&List_GPDMA1_Channel0, NULL, &Node_GPDMA1_Channel0) != HAL_OK)
    {
      Error_Handler();
    }

    if (HAL_DMAEx_List_SetCircularMode(&List_GPDMA1_Channel0) != HAL_OK)
    {
      Error_Handler();
    }

    handle_GPDMA1_Channel0.Instance                         = GPDMA1_Channel0;
    handle_GPDMA1_Channel0.InitLinkedList.Priority          = DMA_LOW_PRIORITY_LOW_WEIGHT;
    handle_GPDMA1_Channel0.InitLinkedList.LinkStepMode      = DMA_LSM_FULL_EXECUTION;
    handle_GPDMA1_Channel0.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0;
    handle_GPDMA1_Channel0.InitLinkedList.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
    handle_GPDMA1_Channel0.InitLinkedList.LinkedListMode    = DMA_LINKEDLIST_CIRCULAR;
    if (HAL_DMAEx_List_Init(&handle_GPDMA1_Channel0) != HAL_OK)
    {
      Error_Handler();
    }

    if (HAL_DMAEx_List_LinkQ(&handle_GPDMA1_Channel0, &List_GPDMA1_Channel0) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(uartHandle, hdmarx, handle_GPDMA1_Channel0);

    if (HAL_DMA_ConfigChannelAttributes(&handle_GPDMA1_Channel0, DMA_CHANNEL_NPRIV) != HAL_OK)
    {
      Error_Handler();
    }

  }

}
