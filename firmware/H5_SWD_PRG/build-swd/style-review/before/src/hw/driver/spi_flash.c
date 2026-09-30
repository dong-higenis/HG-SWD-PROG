#include "spi_flash.h"
#ifdef _USE_HW_SPI_FLASH
#include "qspi/w25q256jv.h"
#include "cli.h"

/* W25Q256JV on the core board, conservative 1-1-1 SDR through OCTOSPI1.
 * Full 32 MiB uses 32-bit addresses after ENTER_4BYTE_MODE.
 * No memory-mapped mode is enabled. Callers use the read/write API. */
static XSPI_HandleTypeDef hospi;
static bool is_init;
static uint8_t jedec_id[3];
static SemaphoreHandle_t flash_mutex;
static void cliCmd(cli_args_t *args);

static bool command(uint8_t opcode, bool addressed, uint32_t address, uint32_t length)
{
  XSPI_RegularCmdTypeDef cmd = {0};
  cmd.OperationType = HAL_XSPI_OPTYPE_COMMON_CFG;
  cmd.IOSelect = HAL_XSPI_SELECT_IO_3_0;
  cmd.Instruction = opcode;
  cmd.InstructionMode = HAL_XSPI_INSTRUCTION_1_LINE;
  cmd.InstructionWidth = HAL_XSPI_INSTRUCTION_8_BITS;
  cmd.AddressMode = addressed ? HAL_XSPI_ADDRESS_1_LINE : HAL_XSPI_ADDRESS_NONE;
  cmd.AddressWidth = HAL_XSPI_ADDRESS_32_BITS;
  cmd.Address = address;
  cmd.DataMode = length ? HAL_XSPI_DATA_1_LINE : HAL_XSPI_DATA_NONE;
  cmd.DataLength = length;
  if (HAL_XSPI_Command(&hospi, &cmd, 100) == HAL_OK) return true;
  HAL_XSPI_Abort(&hospi);
  return false;
}
static bool receive(uint8_t *data, uint32_t timeout)
{
  if (HAL_XSPI_Receive(&hospi, data, timeout) == HAL_OK) return true;
  HAL_XSPI_Abort(&hospi);
  return false;
}
static bool transmit(uint8_t *data, uint32_t timeout)
{
  if (HAL_XSPI_Transmit(&hospi, data, timeout) == HAL_OK) return true;
  HAL_XSPI_Abort(&hospi);
  return false;
}
static bool readStatus(uint8_t *status)
{
  return command(READ_STATUS_REG_CMD, false, 0, 1) && receive(status, 100);
}
static bool waitReady(uint32_t timeout)
{
  uint32_t start = millis();
  do {
    uint8_t status;
    if (!readStatus(&status)) return false;
    if (!(status & W25Q256JV_SR_WIP)) return true;
    delay(1);
  } while (millis()-start < timeout);
  return false;
}
static bool writeEnable(void)
{
  uint8_t status;
  return waitReady(3000) && command(WRITE_ENABLE_CMD, false, 0, 0)
      && readStatus(&status) && (status & W25Q256JV_SR_WREN);
}
static bool rangeValid(uint32_t address, uint32_t length)
{
  return address <= W25Q256JV_FLASH_SIZE && length <= W25Q256JV_FLASH_SIZE-address;
}

void HAL_XSPI_MspInit(XSPI_HandleTypeDef *handle)
{
  if (handle->Instance != OCTOSPI1) return;
  __HAL_RCC_OSPI_CONFIG(RCC_OSPICLKSOURCE_HCLK);
  __HAL_RCC_OSPI1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  GPIO_InitTypeDef pin = {0};
  pin.Mode = GPIO_MODE_AF_PP;
  pin.Pull = GPIO_NOPULL;
  pin.Speed = GPIO_SPEED_FREQ_HIGH;
  pin.Pin = GPIO_PIN_3; /* CLK */
  pin.Alternate = GPIO_AF3_OCTOSPI1;
  HAL_GPIO_Init(GPIOA, &pin);
  pin.Pin = GPIO_PIN_0; /* IO1 */
  pin.Alternate = GPIO_AF6_OCTOSPI1;
  HAL_GPIO_Init(GPIOB, &pin);
  pin.Pin = GPIO_PIN_10; /* NCS */
  pin.Pull = GPIO_PULLUP;
  pin.Alternate = GPIO_AF9_OCTOSPI1;
  HAL_GPIO_Init(GPIOB, &pin);
  pin.Pin = GPIO_PIN_3; /* IO0 */
  pin.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &pin);
  /* IO2/WP and IO3/HOLD stay high in single-line mode. */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_2, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
  pin.Mode = GPIO_MODE_OUTPUT_PP;
  pin.Pin = GPIO_PIN_2;
  HAL_GPIO_Init(GPIOC, &pin);
  pin.Pin = GPIO_PIN_1;
  HAL_GPIO_Init(GPIOA, &pin);
}

bool spiFlashInit(void)
{
  if (!flash_mutex) flash_mutex = xSemaphoreCreateMutex();
  if (!flash_mutex) return false;
  hospi.Instance = OCTOSPI1;
  hospi.Init.FifoThresholdByte = 4;
  hospi.Init.MemoryMode = HAL_XSPI_SINGLE_MEM;
  hospi.Init.MemoryType = HAL_XSPI_MEMTYPE_MICRON;
  hospi.Init.MemorySize = HAL_XSPI_SIZE_256MB;
  hospi.Init.ChipSelectHighTimeCycle = 4;
  hospi.Init.ClockPrescaler = 24; /* 250 MHz / 25 = 10 MHz */
  hospi.Init.DelayBlockBypass = HAL_XSPI_DELAY_BLOCK_BYPASS;
  if (HAL_XSPI_Init(&hospi) != HAL_OK) return false;
  is_init = spiFlashReset();
  if (is_init)
    is_init = command(READ_ID_CMD, false, 0, sizeof(jedec_id)) && receive(jedec_id, 100)
           && jedec_id[0] == 0xEF && jedec_id[1] == 0x40 && jedec_id[2] == 0x19;
  cliAdd("spiFlash", cliCmd);
  logPrintf("[%s] W25Q256JV OCTOSPI1: %02X %02X %02X\n",
            is_init ? "OK" : "E_", jedec_id[0], jedec_id[1], jedec_id[2]);
  return is_init;
}
bool spiFlashIsInit(void) { return is_init; }
bool spiFlashReset(void)
{
  if (!flash_mutex || hospi.Instance != OCTOSPI1) return false;
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = command(RESET_ENABLE_CMD, false, 0, 0) && command(RESET_MEMORY_CMD, false, 0, 0);
  delay(1);
  ok = ok && command(ENTER_4BYTE_MODE, false, 0, 0);
  xSemaphoreGive(flash_mutex);
  return ok;
}
bool spiFlashRead(uint32_t addr, uint8_t *data, uint32_t length)
{
  if (!is_init || !rangeValid(addr, length) || (!data && length)) return false;
  if (!length) return true;
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = waitReady(3000) && command(READ_CMD, true, addr, length) && receive(data, 5000);
  xSemaphoreGive(flash_mutex);
  return ok;
}
bool spiFlashWrite(uint32_t addr, uint8_t *data, uint32_t length)
{
  if (!is_init || !rangeValid(addr, length) || (!data && length)) return false;
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = true;
  while (length && ok) {
    uint32_t chunk = W25Q256JV_PAGE_SIZE - addr % W25Q256JV_PAGE_SIZE;
    if (chunk > length) chunk = length;
    ok = writeEnable() && command(PAGE_PROG_CMD, true, addr, chunk)
      && transmit(data, 100) && waitReady(3000);
    addr += chunk; data += chunk; length -= chunk;
  }
  xSemaphoreGive(flash_mutex);
  return ok;
}
static bool eraseUnit(uint32_t addr, uint32_t size, uint8_t opcode, uint32_t timeout)
{
  if (!is_init || addr % size || !rangeValid(addr, size)) return false;
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = writeEnable() && command(opcode, true, addr, 0) && waitReady(timeout);
  xSemaphoreGive(flash_mutex);
  return ok;
}
bool spiFlashEraseSector(uint32_t addr) { return eraseUnit(addr, 4096, SUBSECTOR_ERASE_CMD, 5000); }
bool spiFlashEraseBlock(uint32_t addr) { return eraseUnit(addr, 65536, SECTOR_ERASE_CMD, 10000); }
bool spiFlashErase(uint32_t addr, uint32_t length)
{
  if (!is_init || addr % 4096 || length % 4096 || !rangeValid(addr, length)) return false;
  while (length) {
    if (!spiFlashEraseSector(addr)) return false;
    addr += 4096; length -= 4096;
  }
  return true;
}
bool spiFlashEraseChip(void)
{
  if (!is_init) return false;
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = writeEnable() && command(BULK_ERASE_CMD, false, 0, 0) && waitReady(300000);
  xSemaphoreGive(flash_mutex);
  return ok;
}
bool spiFlashGetStatus(void)
{
  if (!is_init) return false;
  uint8_t status;
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = readStatus(&status) && !(status & W25Q256JV_SR_WIP);
  xSemaphoreGive(flash_mutex);
  return ok;
}
bool spiFlashGetInfo(spi_flash_info_t *info)
{
  if (!is_init || !info) return false;
  memset(info, 0, sizeof(*info));
  info->FlashSize = W25Q256JV_FLASH_SIZE;
  info->EraseSectorSize = 4096;
  info->EraseSectorsNumber = W25Q256JV_FLASH_SIZE / 4096;
  info->ProgPageSize = W25Q256JV_PAGE_SIZE;
  info->ProgPagesNumber = W25Q256JV_FLASH_SIZE / W25Q256JV_PAGE_SIZE;
  memcpy(info->device_id, jedec_id, sizeof(jedec_id));
  return true;
}
/* Physical window only; do not dereference without enabling memory mapping. */
uint32_t spiFlashGetAddr(void) { return HW_SPI_FLASH_ADDR; }
uint32_t spiFlashGetLength(void) { return W25Q256JV_FLASH_SIZE; }

#ifdef _USE_HW_CLI
void cliCmd(cli_args_t *args)
{
  bool ret = false;
  uint32_t i;
  uint32_t addr;
  uint32_t length;
  uint8_t  data;
  uint32_t pre_time;
  bool flash_ret;


  if (args->argc == 1 && args->isStr(0, "info"))
  {
    cliPrintf("flash addr  : 0x%X\n", 0x0000000);
    ret = true;
  }
  
  if(args->argc == 1 && args->isStr(0, "test"))
  {
    uint8_t rx_buf[256];

    for (int i=0; i<100; i++)
    {
      if (spiFlashRead(0x1000*i, rx_buf, 256))
      {
        cliPrintf("%d : OK\n", i);
      }
      else
      {
        cliPrintf("%d : FAIL\n", i);
        break;
      }
    }
    ret = true;
  }    
  
  if (args->argc == 3 && args->isStr(0, "read"))
  {
    addr   = (uint32_t)args->getData(1);
    length = (uint32_t)args->getData(2);

    for (i=0; i<length; i++)
    {
      flash_ret = spiFlashRead(addr+i, &data, 1);

      if (flash_ret == true)
      {
        cliPrintf( "addr : 0x%X\t 0x%02X\n", addr+i, data);
      }
      else
      {
        cliPrintf( "addr : 0x%X\t Fail\n", addr+i);
      }
    }
    ret = true;
  }
  
  if(args->argc == 3 && args->isStr(0, "erase") == true)
  {
    addr   = (uint32_t)args->getData(1);
    length = (uint32_t)args->getData(2);

    pre_time = millis();
    flash_ret = spiFlashErase(addr, length);

    cliPrintf( "addr : 0x%X\t len : %d %d ms\n", addr, length, (millis()-pre_time));
    if (flash_ret)
    {
      cliPrintf("OK\n");
    }
    else
    {
      cliPrintf("FAIL\n");
    }
    ret = true;
  }

  if(args->argc == 3 && args->isStr(0, "write"))
  {
    uint32_t flash_data;

    addr = (uint32_t)args->getData(1);
    flash_data = (uint32_t )args->getData(2);

    pre_time = millis();
    flash_ret = spiFlashWrite(addr, (uint8_t *)&flash_data, 4);

    cliPrintf( "addr : 0x%X\t 0x%X %dms\n", addr, flash_data, millis()-pre_time);
    if (flash_ret)
    {
      cliPrintf("OK\n");
    }
    else
    {
      cliPrintf("FAIL\n");
    }
    ret = true;
  }

  if (args->argc == 1 && args->isStr(0, "speed-test") == true)
  {
    uint32_t buf[512/4];
    uint32_t cnt;
    uint32_t pre_time;
    uint32_t exe_time;


    cnt = 1024*1024 / 512;
    pre_time = millis();
    for (int i=0; i<cnt; i++)
    {
      if (spiFlashRead(i*512, (uint8_t *)buf, 512) == false)
      {
        cliPrintf("spiFlashRead() Fail:%d\n", i);
        break;
      }
    }
    exe_time = millis()-pre_time;
    if (exe_time > 0)
    {
      cliPrintf("%d KB/sec\n", 1024 * 1000 / exe_time);
    }
    ret = true;
  }

  if (ret == false)
  {
    cliPrintf( "spiFlash info\n");
    cliPrintf( "spiFlash test\n");
    cliPrintf( "spiFlash speed-test\n");
    cliPrintf( "spiFlash read  [addr] [length]\n");
    cliPrintf( "spiFlash erase [addr] [length]\n");
    cliPrintf( "spiFlash write [addr] [data]\n");
  }

}
#endif

#endif