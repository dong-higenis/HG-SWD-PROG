#include "diskio.h"
#include "sd.h"

#define DEV_SD 0

#define SD_TIMEOUT 1000
#define SD_BLOCK_SIZE 512
#define SD_BOUNCE_SECTORS 16  /* 8KB. file_controller 의 1회 flush 최대 크기와 일치 */

static volatile DSTATUS disk_status_flag = STA_NOINIT;
static uint8_t __attribute__((aligned(4))) tmp_buf[SD_BOUNCE_SECTORS * SD_BLOCK_SIZE];


DSTATUS disk_status(BYTE physical_driver_number)
{
  if (physical_driver_number != DEV_SD)
  {
    return STA_NOINIT;
  }

  return disk_status_flag;
}


DSTATUS disk_initialize(BYTE physical_driver_number)
{
  if (physical_driver_number != DEV_SD)
  {
    return STA_NOINIT;
  }

  if (sdIsInit() == true)
  {
    disk_status_flag &= ~STA_NOINIT;
  }
  else
  {
    disk_status_flag = STA_NOINIT;
  }

  return disk_status_flag;
}


DRESULT disk_read(BYTE physical_driver_number, BYTE *read_buffer, LBA_t sector_start_num, UINT sector_count)
{
  if (physical_driver_number != DEV_SD)
  {
    return RES_PARERR;
  }

  if (disk_status_flag & STA_NOINIT)
  {
    return RES_NOTRDY;
  }

  if (read_buffer == NULL)
  {
    return RES_PARERR;
  }

  if (((uint32_t)read_buffer & 0x3) == 0)
  {
    if (sdReadBlocks(sector_start_num, read_buffer, sector_count, SD_TIMEOUT) == false)
    {
      return RES_ERROR;
    }
  }
  else
  {
    /* 비정렬 버퍼는 bounce buffer 크기 단위로 잘라 multi-block 호출 */
    UINT remaining = sector_count;
    UINT done      = 0;
    while (remaining > 0)
    {
      UINT chunk = (remaining > SD_BOUNCE_SECTORS) ? SD_BOUNCE_SECTORS : remaining;
      if (sdReadBlocks(sector_start_num + done, tmp_buf, chunk, SD_TIMEOUT) == false)
      {
        return RES_ERROR;
      }
      memcpy(read_buffer + (done * SD_BLOCK_SIZE), tmp_buf, chunk * SD_BLOCK_SIZE);
      done      += chunk;
      remaining -= chunk;
    }
  }

  return RES_OK;
}


DRESULT disk_write(BYTE physical_driver_number, const BYTE *write_buffer, LBA_t sector_start_num, UINT sector_count)
{
  if (physical_driver_number != DEV_SD)
  {
    return RES_PARERR;
  }

  if (disk_status_flag & STA_NOINIT)
  {
    return RES_NOTRDY;
  }

  if (disk_status_flag & STA_PROTECT)
  {
    return RES_WRPRT;
  }

  if (write_buffer == NULL)
  {
    return RES_PARERR;
  }

  if (((uint32_t)write_buffer & 0x3) == 0)
  {
    if (sdWriteBlocks(sector_start_num, (uint8_t *)write_buffer, sector_count, SD_TIMEOUT) == false)
    {
      return RES_ERROR;
    }
  }
  else
  {
    /* 비정렬 버퍼는 bounce buffer 크기 단위로 잘라 multi-block 호출 */
    UINT remaining = sector_count;
    UINT done      = 0;
    while (remaining > 0)
    {
      UINT chunk = (remaining > SD_BOUNCE_SECTORS) ? SD_BOUNCE_SECTORS : remaining;
      memcpy(tmp_buf, write_buffer + (done * SD_BLOCK_SIZE), chunk * SD_BLOCK_SIZE);
      if (sdWriteBlocks(sector_start_num + done, tmp_buf, chunk, SD_TIMEOUT) == false)
      {
        return RES_ERROR;
      }
      done      += chunk;
      remaining -= chunk;
    }
  }

  return RES_OK;
}


DRESULT disk_ioctl(BYTE physical_driver_number, BYTE cmd, void *io_buff)
{
  DRESULT res = RES_ERROR;
  sd_info_t sd_info;

  if (physical_driver_number != DEV_SD)
  {
    return RES_PARERR;
  }

  if (disk_status_flag & STA_NOINIT)
  {
    return RES_NOTRDY;
  }

  switch (cmd)
  {
    case CTRL_SYNC:
      if (sdIsReady(SD_TIMEOUT) == true)
      {
        res = RES_OK;
      }
      break;

    case GET_SECTOR_COUNT:
      if (sdGetInfo(&sd_info) == true)
      {
        *(LBA_t *)io_buff = sd_info.block_numbers;
        res = RES_OK;
      }
      break;

    case GET_SECTOR_SIZE:
      if (sdGetInfo(&sd_info) == true)
      {
        *(WORD *)io_buff = sd_info.block_size;
        res = RES_OK;
      }
      break;

    case GET_BLOCK_SIZE:
      *(DWORD *)io_buff = 1;
      res = RES_OK;
      break;

    case CTRL_TRIM:
      res = RES_OK;
      break;

    default:
      res = RES_PARERR;
      break;
  }

  return res;
}


DWORD get_fattime(void)
{
  return ((DWORD)(2024 - 1980) << 25)
       | ((DWORD)1 << 21)
       | ((DWORD)1 << 16)
       | ((DWORD)0 << 11)
       | ((DWORD)0 << 5)
       | ((DWORD)0 >> 1);
}