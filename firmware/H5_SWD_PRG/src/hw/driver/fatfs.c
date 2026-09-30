#include "fatfs.h"

#ifdef _USE_HW_FATFS
#include "cli.h"
#include "sd.h"

#ifdef _USE_HW_CLI
void cliFs(cli_args_t *args);
#endif

static FATFS fatfs __attribute__((section(".non_cache"), aligned(32)));
static bool  is_mount = false;

bool fatfsInit(void)
{
  bool ret = false;

  if (sdIsInit() == true)
  {
    ret = fatfsMount();
  }

  logPrintf("[%s] fatfsInit()\n", ret ? "OK" : "E_");

#ifdef _USE_HW_CLI
  cliAdd("fatfs", cliFs);
#endif

  return ret;
}

bool fatfsMount(void)
{
  if (f_mount(&fatfs, "", 1) == FR_OK)
  {
    is_mount = true;
  }
  else
  {
    is_mount = false;
  }
  return is_mount;
}

bool fatfsUnMount(void)
{
  f_mount(NULL, "", 0);
  is_mount = false;
  return true;
}

bool fatfsIsMount(void)
{
  return is_mount;
}

bool fatfsExist(const char *path)
{
  FILINFO fno;

  if (is_mount == false) return false;

  return f_stat(path, &fno) == FR_OK;
}

bool fatfsCreateDir(const char *path)
{
  if (is_mount == false) return false;

  return f_mkdir(path) == FR_OK;
}

bool fatfsDelete(const char *path)
{
  if (is_mount == false) return false;

  return f_unlink(path) == FR_OK;
}

bool fatfsOpen(FIL *p_file, const char *path, uint8_t mode)
{
  if (is_mount == false) return false;

  return f_open(p_file, path, mode) == FR_OK;
}

bool fatfsClose(FIL *p_file)
{
  return f_close(p_file) == FR_OK;
}

bool fatfsSync(FIL *p_file)
{
  return f_sync(p_file) == FR_OK;
}

bool fatfsWrite(FIL *p_file, uint8_t *p_data, uint32_t length)
{
  UINT bw;

  if (f_write(p_file, p_data, length, &bw) == FR_OK)
  {
    return (bw == length);
  }
  return false;
}

bool fatfsRead(FIL *p_file, uint8_t *p_data, uint32_t length, uint32_t *p_read_length)
{
  UINT br;

  if (f_read(p_file, p_data, length, &br) == FR_OK)
  {
    *p_read_length = br;
    return true;
  }
  return false;
}

void cliFs(cli_args_t *args)
{
  bool ret = false;

  if (args->argc == 1 && args->isStr(0, "list") == true)
  {
    DIR     dir;
    FILINFO fno;

    if (f_opendir(&dir, "/") == FR_OK)
    {
      while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0)
      {
        if (fno.fattrib & AM_DIR)
          cliPrintf("[DIR]  %s\n", fno.fname);
        else
          cliPrintf("[FILE] %s  %lu bytes\n", fno.fname, fno.fsize);
      }
      f_closedir(&dir);
    }
    else
    {
      cliPrintf("Failed to open dir\n");
    }
    ret = true;
  }

  if (args->argc == 2 && args->isStr(0, "mkdir") == true)
  {
    const char *path = args->getStr(1);

    if (fatfsCreateDir(path) == true)
      cliPrintf("Created dir : %s\n", path);
    else
      cliPrintf("Failed to create dir : %s\n", path);

    ret = true;
  }

  if (args->argc == 2 && args->isStr(0, "erase") == true)
  {
    const char *path = args->getStr(1);

    if (fatfsDelete(path) == true)
      cliPrintf("Deleted : %s\n", path);
    else
      cliPrintf("Failed to delete : %s\n", path);

    ret = true;
  }

  if (args->argc == 3 && args->isStr(0, "write") == true)
  {
    const char *path = args->getStr(1);
    const char *data = args->getStr(2);
    FIL         file;

    if (fatfsOpen(&file, path, FA_OPEN_APPEND | FA_WRITE))
    {
      if (fatfsWrite(&file, (uint8_t *)data, strlen(data)))
        cliPrintf("Written to : %s\n", path);
      else
        cliPrintf("Failed to write : %s\n", path);

      fatfsClose(&file);
    }
    else
    {
      cliPrintf("Failed to open : %s\n", path);
    }

    ret = true;
  }

  if (ret != true)
  {
    cliPrintf("fatfs list\n");
    cliPrintf("fatfs mkdir <path>\n");
    cliPrintf("fatfs erase <filename>\n");
    cliPrintf("fatfs write <filename> <data>\n");
  }
}
#endif