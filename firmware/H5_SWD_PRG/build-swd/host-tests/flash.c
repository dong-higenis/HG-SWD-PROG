
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define GPIO_AF3_OCTOSPI1 1
#define GPIO_AF6_OCTOSPI1 2
#define GPIO_AF9_OCTOSPI1 3
#define GPIO_MODE_AF_PP 4
#define GPIO_MODE_OUTPUT_PP 5
#define GPIO_NOPULL 6
#define GPIO_PIN_0 7
#define GPIO_PIN_1 8
#define GPIO_PIN_10 9
#define GPIO_PIN_2 10
#define GPIO_PIN_3 11
#define GPIO_PIN_SET 12
#define GPIO_PULLUP 13
#define GPIO_SPEED_FREQ_HIGH 14
#define HAL_XSPI_ADDRESS_1_LINE 15
#define HAL_XSPI_ADDRESS_32_BITS 16
#define HAL_XSPI_ADDRESS_NONE 17
#define HAL_XSPI_DATA_1_LINE 18
#define HAL_XSPI_DATA_NONE 19
#define HAL_XSPI_DELAY_BLOCK_BYPASS 20
#define HAL_XSPI_INSTRUCTION_1_LINE 21
#define HAL_XSPI_INSTRUCTION_8_BITS 22
#define HAL_XSPI_MEMTYPE_MICRON 23
#define HAL_XSPI_OPTYPE_COMMON_CFG 24
#define HAL_XSPI_SELECT_IO_3_0 25
#define HAL_XSPI_SINGLE_MEM 26
#define HAL_XSPI_SIZE_256MB 27
#define RCC_OSPICLKSOURCE_HCLK 28

#define _USE_HW_SPI_FLASH
#define HW_SPI_FLASH_ADDR 0x90000000
#define HAL_OK 0
#define OCTOSPI1 ((void*)1)
#define GPIOA ((void*)2)
#define GPIOB ((void*)3)
#define GPIOC ((void*)4)
#define portMAX_DELAY 0
#define __HAL_RCC_OSPI_CONFIG(x) ((void)0)
#define __HAL_RCC_OSPI1_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
#define HAL_GPIO_Init(a,b) ((void)0)
#define HAL_GPIO_WritePin(a,b,c) ((void)0)
#define cliAdd(a,b) ((void)0)
#define logPrintf(...) ((void)0)
typedef void cli_args_t;
typedef int SemaphoreHandle_t;
typedef struct {uint32_t Pin,Mode,Pull,Speed,Alternate;} GPIO_InitTypeDef;
typedef struct {uint32_t FifoThresholdByte,MemoryMode,MemoryType,MemorySize,ChipSelectHighTimeCycle,ClockPrescaler,DelayBlockBypass;} XSPI_InitTypeDef;
typedef struct {void *Instance;XSPI_InitTypeDef Init;} XSPI_HandleTypeDef;
typedef struct {uint32_t OperationType,IOSelect,Instruction,InstructionMode,InstructionWidth,AddressMode,AddressWidth,Address,DataMode,DataLength;} XSPI_RegularCmdTypeDef;
typedef struct {uint32_t FlashSize,EraseSectorSize,EraseSectorsNumber,ProgPageSize,ProgPagesNumber;uint8_t device_id[20];} spi_flash_info_t;
static XSPI_RegularCmdTypeDef history[10000],last;
static int ncommand,nprogram,fail_next;
static uint32_t chunks[8], addresses[8], tick;
static int xSemaphoreCreateMutex(void) {return 1;}
static void xSemaphoreTake(int a,int b) {(void)a;(void)b;}
static void xSemaphoreGive(int a) {(void)a;}
static uint32_t millis(void) {return tick++;}
static void delay(uint32_t ms) {tick+=ms;}
static int HAL_XSPI_Init(XSPI_HandleTypeDef *p) {return 0;}
static int HAL_XSPI_Abort(XSPI_HandleTypeDef *p) {return 0;}
static int HAL_XSPI_Command(XSPI_HandleTypeDef *p,XSPI_RegularCmdTypeDef *c,int timeout) {
 if(fail_next){fail_next=0;return 1;} last=*c;history[ncommand++]=*c;return 0;
}
static int HAL_XSPI_Receive(XSPI_HandleTypeDef *p,uint8_t *data,int timeout) {
 if(last.Instruction==0x9f){data[0]=0xef;data[1]=0x40;data[2]=0x19;}
 else if(last.Instruction==5)*data=2;
 else memset(data,0xa5,last.DataLength);
 return 0;
}
static int HAL_XSPI_Transmit(XSPI_HandleTypeDef *p,uint8_t *data,int timeout) {
 chunks[nprogram]=last.DataLength;addresses[nprogram++]=last.Address;return 0;
}
bool spiFlashReset(void);
/**
  ******************************************************************************
  * @file    W25Q256JV.h
  * @author  MCD Application Team
  * @version V1.0.0
  * @date    29-May-2015
  * @brief   This file contains all the description of the W25Q256JV QSPI memory.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; COPYRIGHT(c) 2015 STMicroelectronics</center></h2>
  *
  * Redistribution and use in source and binary forms, with or without modification,
  * are permitted provided that the following conditions are met:
  *   1. Redistributions of source code must retain the above copyright notice,
  *      this list of conditions and the following disclaimer.
  *   2. Redistributions in binary form must reproduce the above copyright notice,
  *      this list of conditions and the following disclaimer in the documentation
  *      and/or other materials provided with the distribution.
  *   3. Neither the name of STMicroelectronics nor the names of its contributors
  *      may be used to endorse or promote products derived from this software
  *      without specific prior written permission.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __W25Q256JV_H
#define __W25Q256JV_H

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/

/** @addtogroup BSP
  * @{
  */

/** @addtogroup Components
  * @{
  */

/** @addtogroup W25Q256JV
  * @{
  */

/** @defgroup W25Q256JV_Exported_Types
  * @{
  */

/**
  * @}
  */

/** @defgroup W25Q256JV_Exported_Constants
  * @{
  */

/**
  * @brief  W25Q256JV Configuration
  */
#define W25Q256JV_FLASH_SIZE                  (0x2000000)    /* 256 MBits => 32MBytes */
#define W25Q256JV_SECTOR_SIZE                 0x10000        /* 256 sectors of 64KBytes */
#define W25Q256JV_SUBSECTOR_SIZE              0x1000         /* 4096 subsectors of 4kBytes */
#define W25Q256JV_PAGE_SIZE                   0x100          /* 65536 pages of 256 bytes */

#define W25Q256JV_DUMMY_CYCLES_READ           8
#define W25Q256JV_DUMMY_CYCLES_READ_QUAD      4

#define W25Q256JV_BULK_ERASE_MAX_TIME         250000
#define W25Q256JV_SECTOR_ERASE_MAX_TIME       3000
#define W25Q256JV_SUBSECTOR_ERASE_MAX_TIME    800

/**
  * @brief  W25Q256JV Commands
  */
/* Reset Operations */
#define RESET_ENABLE_CMD                     0x66
#define RESET_MEMORY_CMD                     0x99

/* Identification Operations */
#define READ_ID_CMD                          0x9F
#define READ_ID_CMD2                         0x9F
#define MULTIPLE_IO_READ_ID_CMD              0xAF
#define READ_SERIAL_FLASH_DISCO_PARAM_CMD    0x5A

/* Read Operations */
#define READ_CMD                             0x03
#define FAST_READ_CMD                        0x0B
#define DUAL_OUT_FAST_READ_CMD               0x3B
#define DUAL_INOUT_FAST_READ_CMD             0xBB
#define QUAD_OUT_FAST_READ_CMD               0x6B
#define QUAD_OUT_FAST_READ_CMD_32BITS        0x6C
#define QUAD_INOUT_FAST_READ_CMD             0xEB
#define QUAD_INOUT_FAST_READ_CMD_32BITS      0xEC

/* Write Operations */
#define WRITE_ENABLE_CMD                     0x06
#define WRITE_DISABLE_CMD                    0x04

/* Register Operations */
#define READ_STATUS_REG_CMD                  0x05
#define WRITE_STATUS_REG_CMD                 0x01

#define READ_STATUS_REG2_CMD                 0x35
#define WRITE_STATUS_REG2_CMD                0x31

#define READ_STATUS_REG3_CMD                 0x15
#define WRITE_STATUS_REG3_CMD                0x11


#define READ_LOCK_REG_CMD                    0xE8
#define WRITE_LOCK_REG_CMD                   0xE5

#define READ_FLAG_STATUS_REG_CMD             0x70
#define CLEAR_FLAG_STATUS_REG_CMD            0x50

#define READ_NONVOL_CFG_REG_CMD              0xB5
#define WRITE_NONVOL_CFG_REG_CMD             0xB1

#define READ_VOL_CFG_REG_CMD                 0x85
#define WRITE_VOL_CFG_REG_CMD                0x81

#define READ_ENHANCED_VOL_CFG_REG_CMD        0x65
#define WRITE_ENHANCED_VOL_CFG_REG_CMD       0x61

/* Program Operations */
#define PAGE_PROG_CMD                        0x02
#define DUAL_IN_FAST_PROG_CMD                0xA2
#define EXT_DUAL_IN_FAST_PROG_CMD            0xD2
#define QUAD_IN_FAST_PROG_CMD                0x32
#define QUAD_IN_FAST_PROG_CMD_32BITS         0x34
#define EXT_QUAD_IN_FAST_PROG_CMD            0x12

/* Erase Operations */
#define SUBSECTOR_ERASE_CMD                  0x20
#define SUBSECTOR_ERASE_CMD_32BITS           0x21
#define SECTOR_ERASE_CMD                     0xD8
#define SECTOR_ERASE_CMD_32BITS              0xDC
#define BULK_ERASE_CMD                       0xC7

#define PROG_ERASE_RESUME_CMD                0x7A
#define PROG_ERASE_SUSPEND_CMD               0x75

/* One-Time Programmable Operations */
#define READ_OTP_ARRAY_CMD                   0x4B
#define PROG_OTP_ARRAY_CMD                   0x42

#define ENTER_4BYTE_MODE                     0xB7


/**
  * @brief  W25Q256JV Registers
  */
/* Status Register */
#define W25Q256JV_SR_WIP                      ((uint8_t)0x01)    /*!< Write in progress */
#define W25Q256JV_SR_WREN                     ((uint8_t)0x02)    /*!< Write enable latch */
#define W25Q256JV_SR_BLOCKPR                  ((uint8_t)0x5C)    /*!< Block protected against program and erase operations */
#define W25Q256JV_SR_PRBOTTOM                 ((uint8_t)0x20)    /*!< Protected memory area defined by BLOCKPR starts from top or bottom */
#define W25Q256JV_SR_SRWREN                   ((uint8_t)0x80)    /*!< Status register write enable/disable */

/* Nonvolatile Configuration Register */
#define W25Q256JV_NVCR_LOCK                   ((uint16_t)0x0001) /*!< Lock nonvolatile configuration register */
#define W25Q256JV_NVCR_DUAL                   ((uint16_t)0x0004) /*!< Dual I/O protocol */
#define W25Q256JV_NVCR_QUAB                   ((uint16_t)0x0008) /*!< Quad I/O protocol */
#define W25Q256JV_NVCR_RH                     ((uint16_t)0x0010) /*!< Reset/hold */
#define W25Q256JV_NVCR_ODS                    ((uint16_t)0x01C0) /*!< Output driver strength */
#define W25Q256JV_NVCR_XIP                    ((uint16_t)0x0E00) /*!< XIP mode at power-on reset */
#define W25Q256JV_NVCR_NB_DUMMY               ((uint16_t)0xF000) /*!< Number of dummy clock cycles */

/* Volatile Configuration Register */
#define W25Q256JV_VCR_WRAP                    ((uint8_t)0x03)    /*!< Wrap */
#define W25Q256JV_VCR_XIP                     ((uint8_t)0x08)    /*!< XIP */
#define W25Q256JV_VCR_NB_DUMMY                ((uint8_t)0xF0)    /*!< Number of dummy clock cycles */

/* Enhanced Volatile Configuration Register */
#define W25Q256JV_EVCR_ODS                    ((uint8_t)0x07)    /*!< Output driver strength */
#define W25Q256JV_EVCR_VPPA                   ((uint8_t)0x08)    /*!< Vpp accelerator */
#define W25Q256JV_EVCR_RH                     ((uint8_t)0x10)    /*!< Reset/hold */
#define W25Q256JV_EVCR_DUAL                   ((uint8_t)0x40)    /*!< Dual I/O protocol */
#define W25Q256JV_EVCR_QUAD                   ((uint8_t)0x80)    /*!< Quad I/O protocol */

/* Flag Status Register */
#define W25Q256JV_FSR_PRERR                   ((uint8_t)0x02)    /*!< Protection error */
#define W25Q256JV_FSR_PGSUS                   ((uint8_t)0x04)    /*!< Program operation suspended */
#define W25Q256JV_FSR_VPPERR                  ((uint8_t)0x08)    /*!< Invalid voltage during program or erase */
#define W25Q256JV_FSR_PGERR                   ((uint8_t)0x10)    /*!< Program error */
#define W25Q256JV_FSR_ERERR                   ((uint8_t)0x20)    /*!< Erase error */
#define W25Q256JV_FSR_ERSUS                   ((uint8_t)0x40)    /*!< Erase operation suspended */
#define W25Q256JV_FSR_READY                   ((uint8_t)0x80)    /*!< Ready or command in progress */

/**
  * @}
  */

/** @defgroup W25Q256JV_Exported_Functions
  * @{
  */
/**
  * @}
  */

/**
  * @}
  */

/**
  * @}
  */

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* __W25Q256JV_H */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/

#ifdef _USE_HW_SPI_FLASH

/* W25Q256JV on the core board, conservative 1-1-1 SDR through OCTOSPI1.
 * Full 32 MiB uses 32-bit addresses after ENTER_4BYTE_MODE.
 * No memory-mapped mode is enabled. Callers use the read/write API. */
static XSPI_HandleTypeDef hospi;
static bool               is_init;
static uint8_t            jedec_id[3];
static SemaphoreHandle_t  flash_mutex;
static void               cliSpiFlash(cli_args_t *args);

static bool spiFlashCommand(uint8_t opcode, bool addressed, uint32_t address, uint32_t length)
{
  XSPI_RegularCmdTypeDef cmd = {0};
  cmd.OperationType          = HAL_XSPI_OPTYPE_COMMON_CFG;
  cmd.IOSelect               = HAL_XSPI_SELECT_IO_3_0;
  cmd.Instruction            = opcode;
  cmd.InstructionMode        = HAL_XSPI_INSTRUCTION_1_LINE;
  cmd.InstructionWidth       = HAL_XSPI_INSTRUCTION_8_BITS;
  cmd.AddressMode            = addressed ? HAL_XSPI_ADDRESS_1_LINE : HAL_XSPI_ADDRESS_NONE;
  cmd.AddressWidth           = HAL_XSPI_ADDRESS_32_BITS;
  cmd.Address                = address;
  cmd.DataMode               = length ? HAL_XSPI_DATA_1_LINE : HAL_XSPI_DATA_NONE;
  cmd.DataLength             = length;
  if (HAL_XSPI_Command(&hospi, &cmd, 100) == HAL_OK)
  {
    return true;
  }
  HAL_XSPI_Abort(&hospi);
  return false;
}

static bool spiFlashReceive(uint8_t *data, uint32_t timeout)
{
  if (HAL_XSPI_Receive(&hospi, data, timeout) == HAL_OK)
  {
    return true;
  }
  HAL_XSPI_Abort(&hospi);
  return false;
}

static bool spiFlashTransmit(uint8_t *data, uint32_t timeout)
{
  if (HAL_XSPI_Transmit(&hospi, data, timeout) == HAL_OK)
  {
    return true;
  }
  HAL_XSPI_Abort(&hospi);
  return false;
}

static bool spiFlashReadStatus(uint8_t *status)
{
  return spiFlashCommand(READ_STATUS_REG_CMD, false, 0, 1) && spiFlashReceive(status, 100);
}

static bool spiFlashWaitReady(uint32_t timeout)
{
  uint32_t start = millis();
  do
  {
    uint8_t status;
    if (!spiFlashReadStatus(&status))
    {
      return false;
    }
    if (!(status & W25Q256JV_SR_WIP))
    {
      return true;
    }
    delay(1);
  } while (millis() - start < timeout);
  return false;
}

static bool spiFlashWriteEnable(void)
{
  uint8_t status;
  return spiFlashWaitReady(3000) && spiFlashCommand(WRITE_ENABLE_CMD, false, 0, 0) && spiFlashReadStatus(&status) &&
         (status & W25Q256JV_SR_WREN);
}

static bool spiFlashIsRangeValid(uint32_t address, uint32_t length)
{
  return address <= W25Q256JV_FLASH_SIZE && length <= W25Q256JV_FLASH_SIZE - address;
}

void HAL_XSPI_MspInit(XSPI_HandleTypeDef *handle)
{
  if (handle->Instance != OCTOSPI1)
  {
    return;
  }
  __HAL_RCC_OSPI_CONFIG(RCC_OSPICLKSOURCE_HCLK);
  __HAL_RCC_OSPI1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  GPIO_InitTypeDef pin = {0};
  pin.Mode             = GPIO_MODE_AF_PP;
  pin.Pull             = GPIO_NOPULL;
  pin.Speed            = GPIO_SPEED_FREQ_HIGH;
  pin.Pin              = GPIO_PIN_3; /* CLK */
  pin.Alternate        = GPIO_AF3_OCTOSPI1;
  HAL_GPIO_Init(GPIOA, &pin);
  pin.Pin       = GPIO_PIN_0;        /* IO1 */
  pin.Alternate = GPIO_AF6_OCTOSPI1;
  HAL_GPIO_Init(GPIOB, &pin);
  pin.Pin       = GPIO_PIN_10;       /* NCS */
  pin.Pull      = GPIO_PULLUP;
  pin.Alternate = GPIO_AF9_OCTOSPI1;
  HAL_GPIO_Init(GPIOB, &pin);
  pin.Pin  = GPIO_PIN_3;             /* IO0 */
  pin.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &pin);
  /* IO2/WP and IO3/HOLD stay high in single-line mode. */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_2, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
  pin.Mode = GPIO_MODE_OUTPUT_PP;
  pin.Pin  = GPIO_PIN_2;
  HAL_GPIO_Init(GPIOC, &pin);
  pin.Pin = GPIO_PIN_1;
  HAL_GPIO_Init(GPIOA, &pin);
}

bool spiFlashInit(void)
{
  if (!flash_mutex)
  {
    flash_mutex = xSemaphoreCreateMutex();
  }
  if (!flash_mutex)
  {
    return false;
  }
  hospi.Instance                     = OCTOSPI1;
  hospi.Init.FifoThresholdByte       = 4;
  hospi.Init.MemoryMode              = HAL_XSPI_SINGLE_MEM;
  hospi.Init.MemoryType              = HAL_XSPI_MEMTYPE_MICRON;
  hospi.Init.MemorySize              = HAL_XSPI_SIZE_256MB;
  hospi.Init.ChipSelectHighTimeCycle = 4;
  hospi.Init.ClockPrescaler          = 24; /* 250 MHz / 25 = 10 MHz */
  hospi.Init.DelayBlockBypass        = HAL_XSPI_DELAY_BLOCK_BYPASS;
  if (HAL_XSPI_Init(&hospi) != HAL_OK)
  {
    return false;
  }
  is_init = spiFlashReset();
  if (is_init)
  {
    is_init = spiFlashCommand(READ_ID_CMD, false, 0, sizeof(jedec_id)) && spiFlashReceive(jedec_id, 100) &&
              jedec_id[0] == 0xEF && jedec_id[1] == 0x40 && jedec_id[2] == 0x19;
  }
  cliAdd("spiFlash", cliSpiFlash);
  logPrintf("[%s] W25Q256JV OCTOSPI1: %02X %02X %02X\n", is_init ? "OK" : "E_", jedec_id[0], jedec_id[1], jedec_id[2]);
  return is_init;
}

bool spiFlashIsInit(void)
{
  return is_init;
}

bool spiFlashReset(void)
{
  if (!flash_mutex || hospi.Instance != OCTOSPI1)
  {
    return false;
  }
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = spiFlashCommand(RESET_ENABLE_CMD, false, 0, 0) && spiFlashCommand(RESET_MEMORY_CMD, false, 0, 0);
  delay(1);
  ok = ok && spiFlashCommand(ENTER_4BYTE_MODE, false, 0, 0);
  xSemaphoreGive(flash_mutex);
  return ok;
}

bool spiFlashRead(uint32_t addr, uint8_t *data, uint32_t length)
{
  if (!is_init || !spiFlashIsRangeValid(addr, length) || (!data && length))
  {
    return false;
  }
  if (!length)
  {
    return true;
  }
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = spiFlashWaitReady(3000) && spiFlashCommand(READ_CMD, true, addr, length) && spiFlashReceive(data, 5000);
  xSemaphoreGive(flash_mutex);
  return ok;
}

bool spiFlashWrite(uint32_t addr, uint8_t *data, uint32_t length)
{
  if (!is_init || !spiFlashIsRangeValid(addr, length) || (!data && length))
  {
    return false;
  }
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = true;
  while (length && ok)
  {
    uint32_t chunk = W25Q256JV_PAGE_SIZE - addr % W25Q256JV_PAGE_SIZE;
    if (chunk > length)
    {
      chunk = length;
    }
    ok    = spiFlashWriteEnable() && spiFlashCommand(PAGE_PROG_CMD, true, addr, chunk) && spiFlashTransmit(data, 100) &&
            spiFlashWaitReady(3000);
    addr += chunk;
    data += chunk;
    length -= chunk;
  }
  xSemaphoreGive(flash_mutex);
  return ok;
}

static bool spiFlashEraseUnit(uint32_t addr, uint32_t size, uint8_t opcode, uint32_t timeout)
{
  if (!is_init || addr % size || !spiFlashIsRangeValid(addr, size))
  {
    return false;
  }
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = spiFlashWriteEnable() && spiFlashCommand(opcode, true, addr, 0) && spiFlashWaitReady(timeout);
  xSemaphoreGive(flash_mutex);
  return ok;
}

bool spiFlashEraseSector(uint32_t addr)
{
  return spiFlashEraseUnit(addr, 4096, SUBSECTOR_ERASE_CMD, 5000);
}

bool spiFlashEraseBlock(uint32_t addr)
{
  return spiFlashEraseUnit(addr, 65536, SECTOR_ERASE_CMD, 10000);
}

bool spiFlashErase(uint32_t addr, uint32_t length)
{
  if (!is_init || addr % 4096 || length % 4096 || !spiFlashIsRangeValid(addr, length))
  {
    return false;
  }
  while (length)
  {
    if (!spiFlashEraseSector(addr))
    {
      return false;
    }
    addr   += 4096;
    length -= 4096;
  }
  return true;
}

bool spiFlashEraseChip(void)
{
  if (!is_init)
  {
    return false;
  }
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = spiFlashWriteEnable() && spiFlashCommand(BULK_ERASE_CMD, false, 0, 0) && spiFlashWaitReady(300000);
  xSemaphoreGive(flash_mutex);
  return ok;
}

bool spiFlashGetStatus(void)
{
  if (!is_init)
  {
    return false;
  }
  uint8_t status;
  xSemaphoreTake(flash_mutex, portMAX_DELAY);
  bool ok = spiFlashReadStatus(&status) && !(status & W25Q256JV_SR_WIP);
  xSemaphoreGive(flash_mutex);
  return ok;
}

bool spiFlashGetInfo(spi_flash_info_t *info)
{
  if (!is_init || !info)
  {
    return false;
  }
  memset(info, 0, sizeof(*info));
  info->FlashSize          = W25Q256JV_FLASH_SIZE;
  info->EraseSectorSize    = 4096;
  info->EraseSectorsNumber = W25Q256JV_FLASH_SIZE / 4096;
  info->ProgPageSize       = W25Q256JV_PAGE_SIZE;
  info->ProgPagesNumber    = W25Q256JV_FLASH_SIZE / W25Q256JV_PAGE_SIZE;
  memcpy(info->device_id, jedec_id, sizeof(jedec_id));
  return true;
}

/* Physical window only; do not dereference without enabling memory mapping. */
uint32_t spiFlashGetAddr(void)
{
  return HW_SPI_FLASH_ADDR;
}

uint32_t spiFlashGetLength(void)
{
  return W25Q256JV_FLASH_SIZE;
}

#ifdef _USE_HW_CLI
void cliSpiFlash(cli_args_t *args)
{
  bool     ret = false;
  uint32_t i;
  uint32_t addr;
  uint32_t length;
  uint8_t  data;
  uint32_t pre_time;
  bool     flash_ret;


  if (args->argc == 1 && args->isStr(0, "info"))
  {
    cliPrintf("flash addr  : 0x%X\n", 0x0000000);
    ret = true;
  }

  if (args->argc == 1 && args->isStr(0, "test"))
  {
    uint8_t rx_buf[256];

    for (int i = 0; i < 100; i++)
    {
      if (spiFlashRead(0x1000 * i, rx_buf, 256))
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

    for (i = 0; i < length; i++)
    {
      flash_ret = spiFlashRead(addr + i, &data, 1);

      if (flash_ret == true)
      {
        cliPrintf("addr : 0x%X\t 0x%02X\n", addr + i, data);
      }
      else
      {
        cliPrintf("addr : 0x%X\t Fail\n", addr + i);
      }
    }
    ret = true;
  }

  if (args->argc == 3 && args->isStr(0, "erase") == true)
  {
    addr   = (uint32_t)args->getData(1);
    length = (uint32_t)args->getData(2);

    pre_time  = millis();
    flash_ret = spiFlashErase(addr, length);

    cliPrintf("addr : 0x%X\t len : %d %d ms\n", addr, length, (millis() - pre_time));
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

  if (args->argc == 3 && args->isStr(0, "write"))
  {
    uint32_t flash_data;

    addr       = (uint32_t)args->getData(1);
    flash_data = (uint32_t)args->getData(2);

    pre_time  = millis();
    flash_ret = spiFlashWrite(addr, (uint8_t *)&flash_data, 4);

    cliPrintf("addr : 0x%X\t 0x%X %dms\n", addr, flash_data, millis() - pre_time);
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
    uint32_t buf[512 / 4];
    uint32_t cnt;
    uint32_t pre_time;
    uint32_t exe_time;


    cnt      = 1024 * 1024 / 512;
    pre_time = millis();
    for (int i = 0; i < cnt; i++)
    {
      if (spiFlashRead(i * 512, (uint8_t *)buf, 512) == false)
      {
        cliPrintf("spiFlashRead() Fail:%d\n", i);
        break;
      }
    }
    exe_time = millis() - pre_time;
    if (exe_time > 0)
    {
      cliPrintf("%d KB/sec\n", 1024 * 1000 / exe_time);
    }
    ret = true;
  }

  if (ret == false)
  {
    cliPrintf("spiFlash info\n");
    cliPrintf("spiFlash test\n");
    cliPrintf("spiFlash speed-test\n");
    cliPrintf("spiFlash read  [addr] [length]\n");
    cliPrintf("spiFlash erase [addr] [length]\n");
    cliPrintf("spiFlash write [addr] [data]\n");
  }
}
#endif

#endif

int main(void) {
 assert(spiFlashInit());assert(spiFlashIsInit());
 uint8_t data[258];
 assert(spiFlashRead(0x01000000,data,sizeof(data)));
 assert(last.Address==0x01000000 && last.AddressWidth==HAL_XSPI_ADDRESS_32_BITS);
 for(int i=0;i<258;i++)assert(data[i]==0xa5);
 assert(spiFlashWrite(255,data,258));
 assert(nprogram==3 && chunks[0]==1 && chunks[1]==256 && chunks[2]==1);
 assert(addresses[0]==255 && addresses[1]==256 && addresses[2]==512);
 int before=ncommand;
 assert(!spiFlashRead(0xffffffff,data,2));
 assert(!spiFlashWrite(0x02000000,data,1));
 assert(!spiFlashEraseSector(1));
 assert(!spiFlashErase(0,1));
 assert(!spiFlashRead(0,NULL,1));
 assert(ncommand==before);
 assert(spiFlashErase(0x01000000,8192));
 assert(history[ncommand-2].Address==0x01001000);
 fail_next=1;assert(!spiFlashRead(0,data,1));
 spi_flash_info_t info;assert(spiFlashGetInfo(&info));
 assert(info.FlashSize==0x2000000 && info.ProgPageSize==256 && info.EraseSectorSize==4096);
 puts("PASS flash: >16MiB addressing, page split, overflow/alignment rejection, sector erase, HAL errors");
}
