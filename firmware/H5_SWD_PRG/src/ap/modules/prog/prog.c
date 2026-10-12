#include "module.h"
#include "fatfs.h"
#include "prog.h"
#include "prog_inspect.h"
#include "esp_program.h"
#include "prog_auto.h"
#include "swd/swd_program.h"
#include "lcd/lcd_fonts.h"
#include <math.h>
#include <string.h>

#define PROJECTS    16
#define ID_SIZE     25
#define LOADER_PATH "/prog/loaders/st/0x483.stldr"
#define LOADER_CRC  0x32674EA9U
#define FRAME_MS    25 /* ~30 fps; one I2C OLED frame takes about 25 ms. */
#define SLIDE_MS    140
#define RUN_FRAME_MS 50 /* Progress screen; mostly the bar's two pages change, about 6 ms of I2C each. */
#define BAR_W       116 /* Inside of the progress capsule. */
#define PILL_W      104
#define PILL_H      32
#define PILL_R      7

typedef enum
{
  HOME, /* Auto dashboard; the production entry point. */
  MENU,
  FILES,
  INFO,
  CONFIRM,
  AUTO_SETUP,
  AUTO_FILES,
  ERASE_TARGET,
  ERASE_CONFIRM,
  RUNNING,
  RESULT
} screen_t;

typedef struct
{
  FIL     file;
  FRESULT error;
} sd_file_t;

static struct
{
  screen_t             screen;
  char                 ids[PROJECTS][ID_SIZE];
  unsigned             count;
  unsigned             selected;
  unsigned             menu_selected;
  unsigned             auto_selected;
  unsigned             auto_target;
  bool                 auto_job;
  int                  auto_slot;
  prog_auto_config_t   auto_config;
  char                 auto_names[PROJECTS][48];
  bool                 erase_job;
  unsigned             erase_target;
  bool                 mounted;
  bool                 detected;
  bool                 dirty;
  bool                 confirm_armed;
  bool                 more;
  bool                 cancel;
  bool                 image_open;
  uint32_t             last_buttons;
  uint32_t             hold_start;
  uint32_t             draw_time;
  uint32_t             anim_key; /* Screen state the running animation belongs to. */
  uint32_t             anim_start;
  int                  sel_row; /* Selection bar target for this frame, or -1. */
  int                  sel_from;
  int                  sel_to;
  uint32_t             sel_start;
  screen_t             sel_screen;
  float                bar_px; /* Eased progress fill, px. */
  uint32_t             bar_time;
  uint32_t             job_start;
  uint32_t             done;
  uint32_t             total;
  const char          *stage;
  char                 error[96];
  bool                 esp_project;
  unsigned             esp_file_index;
  esp_project_t        esp;
  esp_program_result_t esp_result;
  prog_manifest_t      manifest;
  prog_loader_t        loader;
  prog_image_t         image;
  swd_program_result_t result;
  sd_file_t            image_file;
  uint32_t             image_size;
  uint32_t             code[1024];
} app;

static void progDraw(void);

static const char *progTargetName(unsigned target)
{
  return target == 0 ? "H723" : espProjectChipName((esp_project_chip_t)(target - 1));
}

static unsigned progCurrentTarget(void)
{
  return app.esp_project ? 1U + (unsigned)app.esp.chip : 0;
}

static bool progAutoReadSlot(unsigned slot, prog_auto_config_t *config)
{
  char path[32];
  snprintf(path, sizeof(path), "/prog/auto%u.cfg", slot);
  FIL file;
  if (f_open(&file, path, FA_READ) != FR_OK)
  {
    return false;
  }
  uint8_t record[PROG_AUTO_RECORD_SIZE];
  UINT    got = 0;
  bool    ok =
  f_size(&file) == sizeof(record) && f_read(&file, record, sizeof(record), &got) == FR_OK && got == sizeof(record);
  FRESULT closed = f_close(&file);
  return ok && closed == FR_OK && progAutoDecode(record, config);
}

static void progAutoLoad(void)
{
  memset(&app.auto_config, 0, sizeof(app.auto_config));
  app.auto_slot = -1;
  if (!app.mounted)
  {
    return;
  }
  for (unsigned slot = 0; slot < 2; slot++)
  {
    prog_auto_config_t config;
    if (progAutoReadSlot(slot, &config) &&
        (app.auto_slot < 0 || progAutoNewer(config.sequence, app.auto_config.sequence)))
    {
      app.auto_config = config;
      app.auto_slot   = (int)slot;
    }
  }
}

static bool progAutoSave(const char *id)
{
  if (!app.mounted || !sdIsDetected())
  {
    return false;
  }
  prog_auto_config_t next = app.auto_config;
  next.sequence++;
  memset(next.project[app.auto_target], 0, PROG_AUTO_ID_SIZE);
  snprintf(next.project[app.auto_target], PROG_AUTO_ID_SIZE, "%s", id);
  uint8_t record[PROG_AUTO_RECORD_SIZE];
  progAutoEncode(&next, record);
  unsigned slot = app.auto_slot == 0 ? 1 : 0;
  char     path[32];
  snprintf(path, sizeof(path), "/prog/auto%u.cfg", slot);
  FIL file;
  if (f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
  {
    return false;
  }
  UINT               written = 0;
  bool               ok      = f_write(&file, record, sizeof(record), &written) == FR_OK && written == sizeof(record);
  FRESULT            synced  = f_sync(&file);
  FRESULT            closed  = f_close(&file);
  prog_auto_config_t checked;
  if (!ok || synced != FR_OK || closed != FR_OK || !progAutoReadSlot(slot, &checked) ||
      checked.sequence != next.sequence || memcmp(checked.project, next.project, sizeof(next.project)))
  {
    return false;
  }
  app.auto_config = next;
  app.auto_slot   = (int)slot;
  return true;
}

static bool progAutoAssigned(void)
{
  for (unsigned target = 0; target < PROG_AUTO_TARGETS; target++)
  {
    if (app.auto_config.project[target][0])
    {
      return true;
    }
  }
  return false;
}

static bool progIsEspErase(void)
{
  return app.erase_job && app.erase_target != 0;
}

static const char *progEraseTargetName(void)
{
  return app.erase_target == 0 ? "H723" : espProjectChipName((esp_project_chip_t)(app.erase_target - 1));
}

static bool progIsValidId(const char *s)
{
  size_t n = strlen(s);
  if (!n || n >= ID_SIZE)
  {
    return false;
  }
  for (; *s; s++)
  {
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
    {
      return false;
    }
  }
  return true;
}

static bool progFail(const char *message)
{
  snprintf(app.error, sizeof(app.error), "%s", message);
  cliPrintf("FAILED   : %s\n", message);
  return false;
}

static bool progIoFail(const char *path, FRESULT error)
{
  snprintf(app.error, sizeof(app.error), "SD error %u", error);
  cliPrintf("FAILED   : %s (FatFs=%u)\n", path, error);
  return false;
}

static bool progReadFile(void *ctx, uint32_t offset, void *buf, uint32_t size)
{
  sd_file_t *s   = ctx;
  UINT       got = 0;
  ledActivityPulse();
  s->error = f_lseek(&s->file, offset);
  if (s->error == FR_OK)
  {
    s->error = f_read(&s->file, buf, size, &got);
  }
  ledActivityPulse();
  if (s->error == FR_OK && got != size)
  {
    s->error = FR_INT_ERR;
  }
  return s->error == FR_OK;
}

static bool progOpenFile(sd_file_t *s, const char *path, prog_file_t *file)
{
  s->error = f_open(&s->file, path, FA_READ);
  if (s->error != FR_OK)
  {
    return progIoFail(path, s->error);
  }
  *file = (prog_file_t){s, (uint32_t)f_size(&s->file), progReadFile};
  return true;
}

static bool progCloseFile(sd_file_t *s)
{
  FRESULT e = f_close(&s->file);
  return e == FR_OK ? true : progIoFail("close", e);
}

static bool progCloseImage(void)
{
  if (!app.image_open)
  {
    return true;
  }
  app.image_open = false;
  return progCloseFile(&app.image_file);
}

static bool progLoadManifest(const char *id)
{
  sd_file_t   s;
  prog_file_t f;
  char        path[96];
  char        text[1025];
  if (!progIsValidId(id))
  {
    return progFail("Invalid project ID");
  }
  snprintf(path, sizeof(path), "/prog/fw/%s/fw.txt", id);
  if (!progOpenFile(&s, path, &f))
  {
    return false;
  }
  const char *error = NULL;
  if (!f.size || f.size > 1024)
  {
    error = "fw.txt size";
  }
  else if (!progReadFile(&s, 0, text, f.size))
  {
    error = "fw.txt read";
  }
  else if (memchr(text, 0, f.size))
  {
    error = "fw.txt NUL";
  }
  else
  {
    text[f.size]    = 0;
    app.esp_project = espProjectIsManifest(text);
    if (app.esp_project)
    {
      error = espProjectParse(text, &app.esp);
      if (!error)
      {
        memset(&app.manifest, 0, sizeof(app.manifest));
        strcpy(app.manifest.name, app.esp.name);
        app.image_size = app.esp.total;
      }
    }
    else
    {
      error = progParseManifest(text, &app.manifest);
    }
  }
  bool closed = progCloseFile(&s);
  if (error)
  {
    return progFail(error);
  }
  return closed;
}

static void progScanProjects(void)
{
  app.count = app.selected = 0;
  app.more                 = false;
  app.error[0]             = 0;
  app.dirty                = true;
  if (!app.mounted)
  {
    return;
  }
  DIR     dir;
  FILINFO info;
  FRESULT e = f_opendir(&dir, "/prog/fw");
  if (e != FR_OK)
  {
    progIoFail("/prog/fw", e);
    return;
  }
  while ((e = f_readdir(&dir, &info)) == FR_OK && info.fname[0])
  {
    if (!(info.fattrib & AM_DIR) || !progIsValidId(info.fname))
    {
      continue;
    }
    char    path[96];
    FILINFO file;
    snprintf(path, sizeof(path), "/prog/fw/%.24s/fw.txt", info.fname);
    if (f_stat(path, &file) != FR_OK || (file.fattrib & AM_DIR))
    {
      continue;
    }
    if (app.count == PROJECTS)
    {
      app.more = true;
      continue;
    }
    strcpy(app.ids[app.count++], info.fname);
  }
  FRESULT ce = f_closedir(&dir);
  if (e != FR_OK || ce != FR_OK)
  {
    app.count = 0;
    progIoFail("scan", e != FR_OK ? e : ce);
  }
  for (unsigned i = 0; i < app.count; i++)
  {
    for (unsigned j = i + 1; j < app.count; j++)
    {
      if (strcmp(app.ids[i], app.ids[j]) > 0)
      {
        char tmp[ID_SIZE];
        strcpy(tmp, app.ids[i]);
        strcpy(app.ids[i], app.ids[j]);
        strcpy(app.ids[j], tmp);
      }
    }
  }
}

static void progAutoProjects(void)
{
  progScanProjects();
  unsigned eligible = 0;
  for (unsigned i = 0; i < app.count; i++)
  {
    if (progLoadManifest(app.ids[i]) && progCurrentTarget() == app.auto_target)
    {
      if (eligible != i)
      {
        strcpy(app.ids[eligible], app.ids[i]);
      }
      strcpy(app.auto_names[eligible], app.manifest.name);
      eligible++;
    }
  }
  app.count = eligible;
  for (unsigned i = 0; i < app.count; i++)
  {
    for (unsigned j = i + 1; j < app.count; j++)
    {
      int order = strcmp(app.auto_names[i], app.auto_names[j]);
      if (order > 0 || (order == 0 && strcmp(app.ids[i], app.ids[j]) > 0))
      {
        char name[48], id[ID_SIZE];
        strcpy(name, app.auto_names[i]);
        strcpy(app.auto_names[i], app.auto_names[j]);
        strcpy(app.auto_names[j], name);
        strcpy(id, app.ids[i]);
        strcpy(app.ids[i], app.ids[j]);
        strcpy(app.ids[j], id);
      }
    }
  }
  app.selected = 0; /* First item clears the binding. */
  for (unsigned i = 0; i < app.count; i++)
  {
    if (!strcmp(app.ids[i], app.auto_config.project[app.auto_target]))
    {
      app.selected = i + 1;
    }
  }
  app.error[0] = 0;
}

static bool progProgress(void *context, const char *step, uint32_t done, uint32_t total)
{
  (void)context;
  app.stage = step;
  app.done  = done;
  app.total = total;
  if (buttonGetPressed(BTN_M) || (!progIsEspErase() && !sdIsDetected()))
  {
    app.cancel = true;
  }
  if ((uint32_t)(millis() - app.draw_time) >= RUN_FRAME_MS)
  {
    app.dirty = true;
    progDraw();
  }
  delay(1);
  return !app.cancel;
}

static bool progReadImage(void *ctx, uint32_t off, void *buf, uint32_t n)
{
  (void)ctx;
  if (!sdIsDetected())
  {
    return progFail("SD removed");
  }
  if (!progReadFile(&app.image_file, off, buf, n))
  {
    return progIoFail("app.bin read", app.image_file.error);
  }
  return true;
}

static bool progLoadLoader(const char *path, uint32_t expected_crc)
{
  if (!sdIsDetected() || !fatfsIsMount())
  {
    return progFail("SD not mounted");
  }
  sd_file_t   s;
  prog_file_t f;
  uint32_t    crc = 0;
  if (!progOpenFile(&s, path, &f))
  {
    return false;
  }
  const char *error = progInspectLoader(&f, &app.loader);
  if (!error && !progFileCrc32(&f, &crc))
  {
    error = "Loader read failed";
  }
  if (!error && (crc != expected_crc || crc != LOADER_CRC))
  {
    error = "Unsupported loader CRC";
  }
  if (!error &&
      (app.loader.code_start != 0x24000004 || app.loader.code_end != 0x240007E8 || app.loader.code_filesz != 2020))
  {
    error = "Unsupported loader layout";
  }
  if (!error && !progReadFile(&s, app.loader.code_offset, app.code, app.loader.code_filesz))
  {
    error = "Loader code read";
  }
  bool closed = progCloseFile(&s);
  if (error)
  {
    return progFail(error);
  }
  if (!closed)
  {
    return false;
  }
  cliPrintf("LOADER   : %s, CRC32=%08lX OK\n", path, (unsigned long)crc);
  return true;
}

static bool progEspPreflight(const char *id)
{
  cliPrintf("TARGET   : %s, %u images, baud ceiling=%lu%s (five-pin UART)\n", espProjectChipName(app.esp.chip),
            app.esp.count, (unsigned long)(app.esp.baud ? app.esp.baud : 921600), app.esp.baud ? "" : " auto");
  for (unsigned i = 0; i < app.esp.count; i++)
  {
    const esp_project_image_t *img = &app.esp.images[i];
    char                       path[128];
    snprintf(path, sizeof(path), "/prog/fw/%s/%s", id, img->file);
    sd_file_t   sd;
    prog_file_t file;
    if (!progOpenFile(&sd, path, &file))
    {
      return false;
    }
    uint32_t    crc        = 0;
    const char *error      = NULL;
    uint8_t     header[24] = {0};
    if (file.size != img->size)
    {
      error = "ESP image size mismatch";
    }
    else if (!progFileCrc32(&file, &crc))
    {
      error = "ESP image read";
    }
    else if (crc != img->crc32)
    {
      error = "ESP image CRC mismatch";
    }
    else if (file.size >= sizeof(header))
    {
      if (!progReadFile(&sd, 0, header, sizeof(header)))
      {
        error = "ESP header read";
      }
      else if (header[0] == 0xE9 && (header[12] | ((uint16_t)header[13] << 8)) != espProjectChipId(app.esp.chip))
      {
        error = "ESP BIN chip mismatch";
      }
    }
    bool closed = progCloseFile(&sd);
    if (error)
    {
      return progFail(error);
    }
    if (!closed)
    {
      return false;
    }
    cliPrintf("IMAGE    : %s @%08lX, %lu B, CRC32=%08lX OK\n", img->file, (unsigned long)img->address,
              (unsigned long)img->size, (unsigned long)crc);
  }
  app.image_size     = app.esp.total;
  app.esp_file_index = UINT32_MAX;
  return true;
}

static bool progReadEspImage(void *context, unsigned image, uint32_t offset, void *data, uint32_t size)
{
  (void)context;
  if (image >= app.esp.count || offset > app.esp.images[image].size || size > app.esp.images[image].size - offset ||
      !sdIsDetected())
  {
    return progFail("ESP SD range/removal");
  }
  if (!app.image_open || app.esp_file_index != image)
  {
    if (!progCloseImage())
    {
      return false;
    }
    char        path[128];
    prog_file_t file;
    snprintf(path, sizeof(path), "/prog/fw/%s/%s", app.ids[app.selected], app.esp.images[image].file);
    if (!progOpenFile(&app.image_file, path, &file))
    {
      return false;
    }
    app.image_open     = true;
    app.esp_file_index = image;
    if (file.size != app.esp.images[image].size)
    {
      progCloseImage();
      return progFail("ESP SD size changed");
    }
  }
  if (!progReadFile(&app.image_file, offset, data, size))
  {
    return progIoFail("ESP image read", app.image_file.error);
  }
  return offset + size == app.esp.images[image].size ? progCloseImage() : true;
}

static void progEspReport(const esp_program_result_t *r)
{
  const char *chip = r->detected == ESP32_CHIP       ? "ESP32"
                     : r->detected == ESP32S3_CHIP   ? "ESP32-S3"
                       : r->detected == ESP32C3_CHIP ? "ESP32-C3"
                                                     : "other/unknown";
  cliPrintf("ESP      : %s at %s; code=%u, chip=%s, flash=%lu B\n", r->error ? r->error : "OK", r->stage,
            (unsigned)r->code, chip, (unsigned long)r->flash_size);
  cliPrintf("BYTES    : written=%lu verified=%lu, address=%08lX, elapsed=%lu ms\n", (unsigned long)r->written,
            (unsigned long)r->verified, (unsigned long)r->address, (unsigned long)r->elapsed_ms);
  cliPrintf("UART RX  : %lu baud, bytes=%lu framing=%lu overflow=%lu late=%lu\n", (unsigned long)r->port.baud,
            (unsigned long)r->port.received, (unsigned long)r->port.framing, (unsigned long)r->port.overflow,
            (unsigned long)r->port.late);
  cliPrintf("LOADER   : %s\n", r->stub ? "RAM stub / 4096-byte blocks" : "ROM");
  cliPrintf("LINK     : test=%lu ms, fallbacks=%u\n", (unsigned long)r->link_test_ms, r->rate_fallbacks);
  cliPrintf("TARGET   : %s; connector released\n", r->started          ? "reset to run firmware"
                                                   : r->erase_complete ? "blank verified; stub downloader"
                                                                       : "no application restart");
}

static bool progPreflight(const char *id)
{
  app.error[0] = 0;
  if (!fatfsIsMount())
  {
    return progFail("SD not mounted");
  }
  if (!progLoadManifest(id))
  {
    return false;
  }
  cliPrintf("PROJECT  : %s\n", app.manifest.name);
  if (app.esp_project)
  {
    return progEspPreflight(id);
  }
  if (!progLoadLoader(app.manifest.algo, app.manifest.loader_crc32))
  {
    return false;
  }
  prog_file_t f;
  uint32_t    crc   = 0;
  const char *error = NULL;
  char        path[128];
  snprintf(path, sizeof(path), "/prog/fw/%s/%s", id, app.manifest.image);
  if (!progOpenFile(&app.image_file, path, &f))
  {
    return false;
  }
  app.image_open = true;
  app.image_size = f.size;
  error          = progInspectImage(&f, app.manifest.address, &app.image);
  if (!error && !progFileCrc32(&f, &crc))
  {
    error = "Image read failed";
  }
  if (!error && crc != app.manifest.crc32)
  {
    error = "Image CRC mismatch";
  }
  if (error)
  {
    progCloseImage();
    return progFail(error);
  }
  cliPrintf("IMAGE    : %s, %lu bytes, CRC32=%08lX OK\n", path, (unsigned long)f.size, (unsigned long)crc);
  cliPrintf("ADDRESS  : %08lX SP=%08lX Reset=%08lX\n", (unsigned long)app.manifest.address, (unsigned long)app.image.sp,
            (unsigned long)app.image.reset);
  return true;
}

static void progFinish(bool ok)
{
  ledActivityStop();
  if (ok)
  {
    app.error[0] = 0;
  }
  buzzerBeep(ok ? 100 : 400);
  app.screen       = RESULT;
  app.dirty        = true;
  app.last_buttons = buttonGetData();
  progDraw();
}

static void progRunJob(void)
{
  app.screen    = RUNNING;
  app.stage     = progIsEspErase() ? "ESP PREFLIGHT" : "CHECK SD FILES";
  app.cancel    = false;
  app.done      = 0;
  app.total     = 1;
  app.job_start = millis();
  memset(&app.result, 0, sizeof(app.result));
  app.dirty = true;
  progDraw();
  app.error[0] = 0;
  if (app.erase_job)
  {
    cliPrintf("ERASE    : %s, entire flash including application and stored data\n", progEraseTargetName());
  }
  bool ok = progIsEspErase() ? true
            : app.erase_job  ? progLoadLoader(LOADER_PATH, LOADER_CRC)
                             : progPreflight(app.ids[app.selected]);
  if (ok && app.auto_job && progCurrentTarget() != app.auto_target)
  {
    ok = progFail("Auto chip mismatch");
  }
  if (ok && !progProgress(NULL, app.stage, 1, 1))
  {
    ok = progFail("Stopped before reset");
  }
  if (ok && (progIsEspErase() || (!app.erase_job && app.esp_project)))
  {
    const esp_program_input_t input = {NULL, progIsEspErase() ? NULL : progReadEspImage, progProgress};
    ok = progIsEspErase() ? espProgramErase((esp_project_chip_t)(app.erase_target - 1), &input, &app.esp_result)
                          : espProgramRun(&app.esp, &input, &app.esp_result);
    app.result.stage          = app.esp_result.stage;
    app.result.address        = app.esp_result.address;
    app.result.written        = app.esp_result.written;
    app.result.verified       = app.esp_result.verified;
    app.result.elapsed_ms     = app.esp_result.elapsed_ms;
    app.result.started        = app.esp_result.started;
    app.result.erase_complete = app.esp_result.erase_complete;
    if (!ok && !app.error[0])
    {
      progFail(app.esp_result.error);
    }
    progEspReport(&app.esp_result);
  }
  else if (ok)
  {
    swd_program_input_t input = {app.code,
                                 app.loader.code_filesz,
                                 app.erase_job ? 0 : app.image_size,
                                 app.erase_job ? 0 : app.manifest.crc32,
                                 NULL,
                                 app.erase_job ? NULL : progReadImage,
                                 progProgress};
    swd_err_t           e     = app.erase_job ? swdEraseH723(&input, &app.result) : swdProgramH723(&input, &app.result);
    ok                        = (e == SWD_OK);
    if (!ok && !app.error[0])
    {
      progFail(app.result.cancelled ? "Stopped" : swdErrStr(e));
    }
    cliPrintf("%s : %s at %s, address=%08lX, SR=%08lX\n", app.erase_job ? "ERASE   " : "PROGRAM ", swdErrStr(e),
              app.result.stage, (unsigned long)app.result.address, (unsigned long)app.result.flash_sr);
    if (!ok)
    {
      cliPrintf("STATUS   : SR=%s; flash status error=%u\n",
                app.result.flash_sr_valid ? "current" : "previous/unavailable",
                (unsigned)app.result.flash_status_error);
      if (app.result.failed_io)
      {
        cliPrintf("FAIL IO  : %s @%08lX ACK=%lu CTRL(last)=%08lX\n", app.result.failed_io,
                  (unsigned long)app.result.failed_io_address, (unsigned long)app.result.failed_ack,
                  (unsigned long)app.result.failed_ctrl);
      }
    }
    cliPrintf("CORE     : PC=%08lX DFSR=%08lX R0=%lu; %s\n", (unsigned long)app.result.pc,
              (unsigned long)app.result.dfsr, (unsigned long)app.result.result,
              app.result.started               ? "new firmware started"
              : app.result.erase_complete      ? "held halted (flash verified blank)"
                : app.result.halted_on_error   ? "held halted"
                  : app.result.reset_attempted ? "state not confirmed"
                                               : "no programming reset");
    cliPrintf("BYTES    : written=%lu verified=%lu; elapsed=%lu s\n", (unsigned long)app.result.written,
              (unsigned long)app.result.verified, (unsigned long)(app.result.elapsed_ms / 1000));
    cliPrintf("SPEED    : clock ceiling=%lu kHz, link test=%lu ms\n", (unsigned long)app.result.clock_khz,
              (unsigned long)app.result.link_test_ms);
    cliPrintf("TIMING   : erase=%lu ms, write=%lu ms, verify=%lu ms\n", (unsigned long)app.result.erase_ms,
              (unsigned long)app.result.write_ms, (unsigned long)app.result.verify_ms);
  }
  if (!progCloseImage())
  {
    ok = false;
  }
  progFinish(ok);
}

static void progRunAuto(void)
{
  app.auto_job  = true;
  app.erase_job = false;
  app.cancel    = false;
  app.error[0]  = 0;
  memset(&app.result, 0, sizeof(app.result));
  app.screen    = RUNNING;
  app.stage     = "AUTO DETECT";
  app.done      = 0;
  app.total     = 1;
  app.job_start = millis();
  app.dirty     = true;
  progDraw();
  if (!app.mounted || !sdIsDetected() || !fatfsIsMount())
  {
    progFail("SD not mounted");
  }
  else if (progProgress(NULL, "DETECT SWD", 0, 1))
  {
    swd_probe_t probe;
    swd_err_t   status = swdProbeH723(100, &probe);
    if (status == SWD_OK)
    {
      app.auto_target = 0;
    }
    else if (status == SWD_ERR_NORESP && (probe.dpidr == 0 || probe.dpidr == UINT32_MAX))
    {
      if (!progProgress(NULL, "DETECT ESP", 0, 1))
      {
        progFail("Stopped");
      }
      else
      {
        espProgramProbe(&app.esp_result);
        progEspReport(&app.esp_result);
        target_chip_t chip = app.esp_result.detected;
        if (app.esp_result.error)
        {
          progFail(app.esp_result.error);
        }
        else if (chip == ESP32_CHIP || chip == ESP32S3_CHIP || chip == ESP32C3_CHIP)
        {
          app.auto_target = chip == ESP32_CHIP ? 1 : chip == ESP32S3_CHIP ? 2 : 3;
        }
        else
        {
          progFail("Unsupported ESP");
        }
      }
    }
    else
    {
      cliPrintf("AUTO SWD : %s, DPIDR=%08lX; ESP fallback refused\n", swdErrStr(status), (unsigned long)probe.dpidr);
      progFail("Unsupported SWD");
    }
    if (!app.error[0] && !progProgress(NULL, "AUTO PROJECT", 0, 1))
    {
      progFail("Stopped");
    }
    if (!app.error[0])
    {
      char id[ID_SIZE];
      strcpy(id, app.auto_config.project[app.auto_target]);
      cliPrintf("AUTO     : %s -> %s\n", progTargetName(app.auto_target), id[0] ? id : "(not assigned)");
      if (!id[0])
      {
        progFail("Set Auto project");
      }
      else
      {
        /* Use the saved ID directly; a change in directory sort order cannot select another package. */
        app.selected = 0;
        strcpy(app.ids[0], id);
        progRunJob();
        return;
      }
    }
  }
  else
  {
    progFail("Stopped");
  }
  progFinish(false);
}

/* Pixel-aligned artwork for the 128x64 monochrome OLED. */
static const uint16_t icon_flash[12] = {0x060, 0x0C0, 0x180, 0x300, 0x7F0, 0xFE0, 0x0C0, 0x180, 0x300, 0x600, 0x400, 0};
static const uint16_t icon_erase[12] = {0x0F0, 0x3FC, 0x000, 0x204, 0x294, 0x294,
                                        0x294, 0x294, 0x294, 0x204, 0x204, 0x3FC};
static const uint16_t icon_info[12]  = {0x1F8, 0x204, 0x402, 0x861, 0x801, 0x861,
                                        0x861, 0x861, 0x861, 0x402, 0x204, 0x1F8};
static const uint16_t icon_card[12]  = {0x1FE, 0x202, 0x422, 0x8AA, 0x8AA, 0x802,
                                        0x802, 0x802, 0x802, 0x802, 0x802, 0xFFE};
static const uint16_t icon_setup[12] = {0x060, 0xFFF, 0x060, 0, 0x600, 0xFFF, 0x600, 0, 0x00C, 0xFFF, 0x00C, 0};

static void progDrawIcon(int x, int y, const uint16_t *rows, uint16_t color, unsigned scale)
{
  for (unsigned r = 0; r < 12; r++)
  {
    for (unsigned c = 0; c < 12; c++)
    {
      if (rows[r] & (1U << (11 - c)))
      {
        lcdDrawFillRect(x + c * scale, y + r * scale, scale, scale, color);
      }
    }
  }
}

static void progDrawHeader(const char *title)
{
  lcdPrintf(2, 0, white, "%s", title);
  lcdDrawFillRect(0, 12, 128, 1, white);
}

static void progDrawFooter(const char *left, const char *right)
{
  lcdDrawFillRect(0, 52, 128, 1, white);
  lcdPrintf(2, 54, white, "%s", left);
  lcdPrintf(126 - (int)strlen(right) * 7, 54, white, "%s", right);
}

/* Lists draw every row in white and only name the selected row; the bar is inverted on top at the end of the frame. */
static void progDrawSelection(int y, int height)
{
  (void)height;
  app.sel_row = y;
}

static int progSlideAt(uint32_t now)
{
  uint32_t t = now - app.sel_start;
  if (t >= SLIDE_MS)
  {
    return app.sel_to;
  }
  float p = 1.0f - (float)t / SLIDE_MS; /* Ease-out cubic. */
  return app.sel_to + (int)lroundf((float)(app.sel_from - app.sel_to) * p * p * p);
}

/* XOR keeps the text readable while the bar is between two rows. */
static void progInvertSelection(void)
{
  uint32_t now = millis();
  if (app.sel_screen != app.screen)
  {
    app.sel_from = app.sel_to = app.sel_row; /* A newly opened list starts on its row. */
  }
  else if (app.sel_row != app.sel_to)
  {
    app.sel_from  = progSlideAt(now);
    app.sel_to    = app.sel_row;
    app.sel_start = now;
  }
  int y = progSlideAt(now);
  for (int r = 0; r < 12; r++)
  {
    bool edge = r == 0 || r == 11;
    for (int x = edge ? 2 : 1; x <= (edge ? 121 : 122); x++)
    {
      if (y + r >= 0 && y + r < LCD_HEIGHT)
      {
        lcdDrawPixel(x, y + r, lcdReadPixel(x, y + r) == white ? black : white);
      }
    }
  }
}

static void progDrawSdBadge(void)
{
  if (app.mounted)
  {
    lcdDrawFillRect(104, 0, 24, 11, white);
  }
  else
  {
    lcdDrawRect(104, 0, 24, 11, white);
  }
  lcdPrintf(109, 0, app.mounted ? black : white, "SD");
}

typedef enum
{
  ANIM_NONE,
  ANIM_DROP,  /* READY: falls in, splats, then jiggles like jelly now and then. */
  ANIM_POP,   /* Springs up from nothing with a little overshoot. */
  ANIM_BURST, /* POP plus a ring of sparks: a good result. */
  ANIM_SHAKE  /* Head shake: a bad result. */
} anim_kind_t;

typedef struct
{
  float x, y;   /* Offset from rest, px. */
  float sx, sy; /* Scale about the anchor. */
  float shear;  /* Sideways lean of the top edge, px; the bottom stays put. */
  float t;      /* Seconds since the state was entered. */
  bool  bottom; /* Scale about the bottom edge instead of the centre. */
  bool  key;    /* Keycap looks pressed. */
  bool  active; /* Still moving; keep redrawing. */
} anim_t;

static anim_kind_t progAnimKind(void)
{
  if (app.screen == HOME)
  {
    return app.mounted && progAutoAssigned() ? ANIM_DROP : ANIM_POP;
  }
  if (app.screen == RESULT)
  {
    return app.error[0] ? ANIM_SHAKE : ANIM_BURST;
  }
  return ANIM_NONE;
}

/* Animation pose for the current screen; the clock restarts whenever the screen state changes. */
static anim_t progAnim(void)
{
  anim_kind_t kind = progAnimKind();
  uint32_t    key  = (uint32_t)app.screen << 8 | (uint32_t)kind << 4 | (app.screen == HOME && app.mounted);
  if (key != app.anim_key)
  {
    app.anim_key   = key;
    app.anim_start = millis();
  }
  anim_t a = {0, 0, 1, 1, 0, (float)(millis() - app.anim_start) / 1000.0f, false, false, false};
  float  t = a.t;
  if (kind == ANIM_DROP)
  {
    a.bottom = true;
    if (t < 0.22f)
    {
      float p  = t / 0.22f; /* Free fall, stretched by the speed. */
      a.y      = -48.0f * (1.0f - p * p);
      a.sx     = 0.92f;
      a.sy     = 1.1f;
      a.active = true;
    }
    else if (t < 1.6f)
    {
      float u  = t - 0.22f; /* Landing splat: squash first, then settle. */
      float s  = 0.24f * expf(-u / 0.2f) * cosf(6.2832f * 3.0f * u);
      a.sx     = 1.0f + s;
      a.sy     = 1.0f / (1.0f + s);
      a.active = true;
    }
    else
    {
      float u  = fmodf(t - 1.6f, 3.2f); /* A poke every 3.2 s: wobble and lean, keeping its volume. */
      float s  = 0.11f * expf(-u / 0.25f) * sinf(6.2832f * 3.2f * u);
      a.sx     = 1.0f + s;
      a.sy     = 1.0f / (1.0f + s);
      a.shear  = 5.0f * expf(-u / 0.35f) * sinf(6.2832f * 2.2f * u);
      a.key    = u < 0.12f;
      a.active = u < 1.3f;
    }
  }
  else if (kind == ANIM_POP || kind == ANIM_BURST)
  {
    a.sx = a.sy = 1.0f - expf(-t / 0.12f) * cosf(6.2832f * 2.2f * t);
    a.active    = t < 0.9f;
  }
  else if (kind == ANIM_SHAKE)
  {
    a.x      = 7.0f * expf(-t / 0.18f) * sinf(6.2832f * 7.0f * t);
    a.active = t < 0.8f;
  }
  return a;
}

/* Rounded-corner inset of row j in a pill of height h. */
static int progPillInset(int j, int h, float r)
{
  float d = j < r ? r - j - 0.5f : j >= h - r ? j - (h - r) + 0.5f : 0.0f;
  return d > 0 ? (int)(r - sqrtf(fmaxf(0.0f, r * r - d * d)) + 0.5f) : 0;
}

static int progMax(int a, int b)
{
  return a > b ? a : b;
}

static int progMin(int a, int b)
{
  return a < b ? a : b;
}

/* One big word in a pill, filled when the news is good or SEL starts a job, outlined otherwise.
 * Rasterised row by row so it can squash, stretch and lean; pixels off the screen are dropped. */
static void progDrawStatus(int y, const char *word, bool filled, const anim_t *a)
{
  int w = (int)(PILL_W * a->sx + 0.5f);
  int h = progMin((int)(PILL_H * a->sy + 0.5f), LCD_HEIGHT);
  if (w < 4 || h < 4)
  {
    return;
  }
  float cx  = 64.0f + a->x;
  float top = y + a->y + (a->bottom ? PILL_H - PILL_H * a->sy : (PILL_H - PILL_H * a->sy) / 2);
  int   y0  = (int)floorf(top + 0.5f);
  float r   = PILL_R * fminf(a->sx, a->sy);
  int   xl[LCD_HEIGHT], xr[LCD_HEIGHT];
  float shift[LCD_HEIGHT];
  for (int j = 0; j < h; j++)
  {
    float k  = 1.0f - (float)j / (h - 1);
    shift[j] = a->shear * k * k;
    int left = (int)floorf(cx - w / 2.0f + shift[j] + 0.5f);
    int in   = progPillInset(j, h, r);
    xl[j]    = left + in;
    xr[j]    = left + w - 1 - in;
  }
  for (int j = 0; j < h; j++)
  {
    int from = xl[j], to = xr[j];
    if (filled || j == 0 || j == h - 1)
    {
      for (int x = from; x <= to; x++)
      {
        lcdDrawPixel(x, y0 + j, white);
      }
      continue;
    }
    /* Outline: stretch each edge pixel towards its neighbours so the curve has no gaps. */
    int le = progMax(from, progMax(xl[j - 1], xl[j + 1]) - 1);
    int rs = progMin(to, progMin(xr[j - 1], xr[j + 1]) + 1);
    for (int x = from; x <= le; x++)
    {
      lcdDrawPixel(x, y0 + j, white);
    }
    for (int x = rs; x <= to; x++)
    {
      lcdDrawPixel(x, y0 + j, white);
    }
  }
  /* The 16x26 glyphs, sampled through the same transform. */
  int      n     = (int)strlen(word);
  float    tw    = n * 16 * a->sx;
  float    ty    = 4.0f * a->sy;
  uint16_t color = filled ? black : white;
  for (int j = 0; j < h; j++)
  {
    int v = j >= ty ? (int)((j - ty) / a->sy) : -1;
    if (v < 0 || v >= 26)
    {
      continue;
    }
    float left = cx - tw / 2.0f + shift[j];
    for (int x = (int)ceilf(left); x <= (int)(left + tw); x++)
    {
      int u = (int)((x - left) / a->sx);
      if (u < 0 || u >= n * 16)
      {
        continue;
      }
      uint16_t bits = font_16x26.data[(word[u / 16] - 32) * 26 + v];
      if ((bits << (u % 16)) & 0x8000)
      {
        lcdDrawPixel(x, y0 + j, color);
      }
    }
  }
}

/* Small plus-shaped sparks flying out of the pill, shrinking as they go. */
static void progDrawSparks(int cy, float t)
{
  float p = (t - 0.12f) / 0.5f;
  if (p <= 0 || p >= 1)
  {
    return;
  }
  float d   = 16.0f * (1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p));
  int   arm = p < 0.5f ? 2 : p < 0.8f ? 1 : 0;
  for (int k = 0; k < 8; k++)
  {
    float angle = k * 0.7854f + 0.3927f;
    int   x     = (int)lroundf(64.0f + cosf(angle) * (PILL_W / 2 + 4 + d));
    int   y     = (int)lroundf(cy + sinf(angle) * (PILL_H / 2 + 2 + d * 0.45f));
    if (y < 1 || y > 48)
    {
      continue; /* Keep clear of the hint line. */
    }
    lcdDrawPixel(x, y, white);
    for (int q = 1; q <= arm; q++)
    {
      lcdDrawPixel(x + q, y, white);
      lcdDrawPixel(x - q, y, white);
      lcdDrawPixel(x, y + q, white);
      lcdDrawPixel(x, y - q, white);
    }
  }
}

/* One centred line of small text, clipped to the screen width. */
static void progDrawCentered(int y, const char *text)
{
  int n = (int)strlen(text) > 18 ? 18 : (int)strlen(text);
  lcdPrintf(64 - n * 7 / 2, y, white, "%.*s", n, text);
}

/* Centred bottom hint, with the button drawn as a small keycap. */
static void progDrawHint(const char *key, const char *text, bool pressed)
{
  int key_w = key ? (int)strlen(key) * 7 + 6 : 0;
  int x     = 64 - (key_w + (key ? 5 : 0) + (int)strlen(text) * 7) / 2;
  if (key)
  {
    int down = pressed ? 1 : 0; /* A pressed key sinks one pixel. */
    lcdDrawFillRoundRect(x, 52 + down, key_w, 12 - down, 2, white);
    lcdPrintf(x + 3, 53 + down, black, "%s", key);
    x += key_w + 5;
  }
  lcdPrintf(x, 53, white, "%s", text);
}

/* Capsule progress bar: eased fill with diagonal stripes marching through it. */
static void progDrawProgress(int y, unsigned pct, bool busy)
{
  uint32_t now = millis();
  float    dt  = (float)(now - app.bar_time) / 1000.0f;
  float    t   = (float)(now - app.job_start) / 1000.0f;
  float    to  = pct * (float)BAR_W / 100.0f;
  app.bar_time = now;
  if (busy || to < app.bar_px - 1.0f || dt > 0.5f)
  {
    app.bar_px = to; /* New job or a stage restarting from zero: no backwards glide. */
  }
  else
  {
    app.bar_px += (to - app.bar_px) * (1.0f - expf(-dt / 0.15f));
  }
  int from = 0, width = (int)(app.bar_px + 0.5f);
  if (busy)
  {
    width = 28; /* Ping-pong marker with eased turns. */
    from  = (int)((1.0f - cosf(t * 3.1416f / 0.9f)) / 2.0f * (BAR_W - width) + 0.5f);
  }
  lcdDrawRoundRect(4, y, BAR_W + 8, 10, 4, white);
  int march = (int)(t * 24.0f);
  for (int x = from; x < from + width && x < BAR_W; x++)
  {
    bool end = x == from || x == from + width - 1;
    for (int r = end ? 1 : 0; r < (end ? 5 : 6); r++) /* Clip the corners to round the ends. */
    {
      if (((x - r - march) % 8 + 8) % 8 < 6)
      {
        lcdDrawPixel(8 + x, y + 2 + r, white);
      }
    }
  }
}

static void progDraw(void)
{
  if (!app.dirty || !lcdIsInit())
  {
    return;
  }
  app.dirty     = false;
  app.draw_time = millis();
  lcdSetFont(LCD_FONT_07x10);
  lcdClearBuffer(black);
  app.sel_row = -1;
  anim_t anim = progAnim();
  if (app.screen == HOME)
  {
    bool ready = app.mounted && progAutoAssigned();
    progDrawStatus(14, !app.mounted ? "NO SD" : ready ? "READY" : "SETUP", ready, &anim);
    if (!app.mounted)
    {
      progDrawHint(NULL, app.detected ? "Check FAT32 card" : "Insert SD card", false);
    }
    else
    {
      progDrawHint("SEL", ready ? "start" : "set up", anim.key);
    }
  }
  else if (app.screen == MENU)
  {
    progDrawHeader("Menu");
    progDrawSdBadge();
    const char     *items[] = {"Auto setup", "Flash", "Erase", "Info"};
    const uint16_t *icons[] = {icon_setup, icon_flash, icon_erase, icon_info};
    unsigned        first   = app.menu_selected > 2 ? app.menu_selected - 2 : 0;
    for (unsigned row = 0; row < 3; row++)
    {
      int      y     = 15 + row * 12;
      unsigned item  = first + row;
      bool     sel   = item == app.menu_selected;
      if (sel)
      {
        progDrawSelection(y, 12);
        lcdPrintf(111, y + 1, white, ">");
      }
      progDrawIcon(8, y, icons[item], white, 1);
      lcdPrintf(29, y + 1, white, "%s", items[item]);
    }
    progDrawFooter("SEL:Open", "M:Auto");
  }
  else if (app.screen == INFO)
  {
    progDrawHeader("H5-SWD-Prog");
    lcdPrintf(39, 16, white, "made by");
    lcdSetFont(LCD_FONT_11x18);
    lcdPrintf(25, 29, white, "Higenis");
    lcdSetFont(LCD_FONT_07x10);
    progDrawFooter("Info", "M:Back");
  }
  else if (app.screen == FILES)
  {
    progDrawHeader("Flash");
    if (!app.mounted)
    {
      progDrawIcon(5, 21, icon_card, white, 2);
      lcdPrintf(39, 19, white, app.detected ? "SD not ready" : "No SD card");
      lcdPrintf(39, 34, white, app.detected ? "Check FAT32" : "Insert card");
    }
    else if (!app.count)
    {
      lcdPrintf(29, 19, white, "No firmware");
      lcdPrintf(2, 35, white, "%.17s", app.error[0] ? app.error : "Add files to SD");
    }
    else
    {
      char count[24];
      snprintf(count, sizeof(count), "%u/%u", app.selected + 1, app.count);
      lcdPrintf(126 - (int)strlen(count) * 7, 0, white, "%s", count);
      unsigned first = (app.selected / 3) * 3;
      for (unsigned row = 0; row < 3 && first + row < app.count; row++)
      {
        bool sel = first + row == app.selected;
        int  y   = 15 + row * 12;
        if (sel)
        {
          progDrawSelection(y, 12);
        }
        lcdPrintf(5, y + 1, white, "%c%.15s", sel ? '>' : ' ', app.ids[first + row]);
      }
      if (app.count > 3)
      {
        lcdDrawFillRect(126, 15, 1, 36, white);
        unsigned height = 36 * 3 / app.count;
        unsigned y      = 15 + (36 - height) * app.selected / (app.count - 1);
        lcdDrawFillRect(125, y, 3, height, white);
      }
    }
    progDrawFooter(app.mounted && app.count ? "SEL:Open" : "", "M:Back");
  }
  else if (app.screen == AUTO_SETUP)
  {
    progDrawHeader("Auto setup");
    lcdPrintf(105, 0, white, "%u/%u", app.auto_selected + 1, PROG_AUTO_TARGETS);
    unsigned first = app.auto_selected > 2 ? app.auto_selected - 2 : 0;
    for (unsigned row = 0; row < 3 && first + row < PROG_AUTO_TARGETS; row++)
    {
      unsigned    item     = first + row;
      bool        selected = item == app.auto_selected;
      int         y        = 15 + row * 12;
      const char *id       = app.auto_config.project[item];
      if (selected)
      {
        progDrawSelection(y, 12);
      }
      lcdPrintf(4, y + 1, white, "%-8s %.8s", progTargetName(item), id[0] ? id : "---");
    }
    progDrawFooter("SEL:Set", "M:Back");
  }
  else if (app.screen == AUTO_FILES)
  {
    progDrawHeader(progTargetName(app.auto_target));
    char position[12];
    snprintf(position, sizeof(position), "%u/%u", app.selected + 1, app.count + 1);
    lcdPrintf(126 - (int)strlen(position) * 7, 0, white, "%s", position);
    if (!app.mounted)
    {
      lcdPrintf(2, 20, white, "Insert SD card");
    }
    else
    {
      unsigned first = (app.selected / 3) * 3;
      for (unsigned row = 0; row < 3 && first + row <= app.count; row++)
      {
        unsigned item     = first + row;
        bool     selected = item == app.selected;
        int      y        = 15 + row * 12;
        if (selected)
        {
          progDrawSelection(y, 12);
        }
        const char *name   = item ? app.auto_names[item - 1] : "(None)";
        unsigned    length = strlen(name);
        if (selected && length > 15)
        {
          unsigned offset = (millis() / 400) % (length - 15 + 6);
          offset          = offset < 3 ? 0 : offset - 3;
          if (offset > length - 15)
          {
            offset = length - 15;
          }
          name += offset;
        }
        lcdPrintf(5, y + 1, white, "%c%.15s", selected ? '>' : ' ', name);
      }
    }
    progDrawFooter(app.error[0] ? "Save failed" : "SEL:Save", "M:Back");
  }
  else if (app.screen == ERASE_TARGET)
  {
    progDrawHeader("Erase / Target");
    const char *items[] = {"H723", "ESP32", "ESP32-S3", "ESP32-C3"};
    unsigned    first   = app.erase_target > 2 ? app.erase_target - 2 : 0;
    for (unsigned row = 0; row < 3 && first + row < 4; row++)
    {
      bool selected = first + row == app.erase_target;
      int  y        = 15 + row * 12;
      if (selected)
      {
        progDrawSelection(y, 12);
      }
      lcdPrintf(5, y + 1, white, "%c%s", selected ? '>' : ' ', items[first + row]);
    }
    lcdPrintf(105, 0, white, "%u/4", app.erase_target + 1);
    progDrawFooter("SEL:Open", "M:Back");
  }
  else if (app.screen == ERASE_CONFIRM)
  {
    char title[24];
    snprintf(title, sizeof(title), "Erase / %s", progEraseTargetName());
    progDrawHeader(title);
    if (!app.mounted && !progIsEspErase())
    {
      progDrawIcon(5, 21, icon_card, white, 2);
      lcdPrintf(39, 19, white, app.detected ? "SD not ready" : "No SD card");
      lcdPrintf(39, 34, white, "Loader on SD");
      progDrawFooter("", "M:Back");
    }
    else
    {
      lcdPrintf(2, 16, white, progIsEspErase() ? "ALL FLASH" : "ALL FLASH: 1 MiB");
      lcdPrintf(2, 28, white, progIsEspErase() ? "Size: auto-detect" : "Erase 8 sectors");
      lcdPrintf(2, 40, white, progIsEspErase() ? "App + data lost" : "Deletes firmware");
      progDrawFooter("SEL:Hold 1s", "M:Back");
    }
  }
  else if (app.screen == CONFIRM)
  {
    progDrawHeader("Flash / Confirm");
    lcdPrintf(2, 16, white, "%.17s", app.manifest.name);
    lcdPrintf(2, 28, white, "%lu B", (unsigned long)app.image_size);
    if (app.esp_project)
    {
      lcdPrintf(2, 40, white, "%s / %u BIN", espProjectChipName(app.esp.chip), app.esp.count);
    }
    else
    {
      lcdPrintf(2, 40, white, "Erase %lu sectors", (unsigned long)((app.image_size + 0x1FFFF) / 0x20000));
    }
    progDrawFooter("SEL:Hold 1s", "M:Back");
  }
  else if (app.screen == RUNNING)
  {
    unsigned pct = app.total ? (unsigned)(((uint64_t)app.done * 100) / app.total) : 0;
    if (pct > 100)
    {
      pct = 100;
    }
    unsigned long seconds = (millis() - app.job_start) / 1000;
    if (seconds > 99999)
    {
      seconds = 99999;
    }
    char text[12];
    snprintf(text, sizeof(text), "%lus", seconds);
    lcdPrintf(2, 0, white, "%.*s", 17 - (int)strlen(text), app.stage);
    lcdPrintf(126 - (int)strlen(text) * 7, 0, white, "%s", text);
    snprintf(text, sizeof(text), "%u%%", pct);
    lcdSetFont(LCD_FONT_16x26);
    lcdPrintf(64 - (int)strlen(text) * 8, 12, white, "%s", text);
    lcdSetFont(LCD_FONT_07x10);
    /* No progress yet (preflight, probing, a long erase) shows a sweeping marker instead of an empty bar. */
    progDrawProgress(40, pct, app.done == 0);
    progDrawHint(app.cancel ? NULL : "M", app.cancel ? "Stopping..." : "stop", false);
  }
  else
  {
    /* Big verdict that reads at arm's length; a failure also names its cause (details on the CLI). */
    bool ok = !app.error[0];
    if (ok)
    {
      progDrawStatus(14, app.auto_job ? "PASS" : "DONE", true, &anim);
      progDrawSparks(30, anim.t);
    }
    else
    {
      progDrawStatus(4, "FAIL", false, &anim);
      progDrawCentered(39, app.error);
    }
    progDrawHint("SEL", app.auto_job ? (ok ? "home" : "retry") : "back", false);
  }
  if (app.sel_row >= 0)
  {
    progInvertSelection();
  }
  app.sel_screen = app.screen;
  lcdUpdateDraw();
}

void progUpdate(void)
{
  bool detected = sdIsDetected(), mounted = detected && fatfsIsMount();
  if (mounted != app.mounted || detected != app.detected)
  {
    app.mounted  = mounted;
    app.detected = detected;
    app.dirty    = true;
    progAutoLoad();
    /* Keep the result visible even when the card was removed during a job. */
    if (app.screen != RESULT)
    {
      if (app.screen == AUTO_FILES)
      {
        progAutoProjects();
      }
      else
      {
        progScanProjects();
      }
    }
    if (app.screen == CONFIRM)
    {
      app.screen = FILES;
    }
    app.confirm_armed = false;
    app.hold_start    = 0;
  }
  uint32_t buttons = buttonGetData(), pressed = buttons & ~app.last_buttons;
  app.last_buttons = buttons;
  if (app.screen == HOME)
  {
    if (pressed & (1U << BTN_M))
    {
      app.screen = MENU;
      app.dirty  = true;
    }
    else if ((pressed & (1U << BTN_SEL)) && app.mounted)
    {
      if (progAutoAssigned())
      {
        progRunAuto();
      }
      else
      {
        app.auto_selected = 0;
        app.screen        = AUTO_SETUP;
        app.dirty         = true;
      }
    }
  }
  else if (app.screen == MENU)
  {
    if (pressed & (1U << BTN_M))
    {
      app.screen = HOME;
      app.dirty  = true;
    }
    if (pressed & (1U << BTN_UP))
    {
      app.menu_selected = (app.menu_selected + 3) % 4;
      app.dirty         = true;
    }
    if (pressed & (1U << BTN_DN))
    {
      app.menu_selected = (app.menu_selected + 1) % 4;
      app.dirty         = true;
    }
    if (pressed & (1U << BTN_SEL))
    {
      app.auto_job      = false;
      app.erase_job     = app.menu_selected == 2;
      app.screen        = app.menu_selected == 0     ? AUTO_SETUP
                          : app.menu_selected == 1   ? FILES
                            : app.menu_selected == 2 ? ERASE_TARGET
                                                     : INFO;
      app.confirm_armed = false;
      app.hold_start    = 0;
      if (app.screen == FILES)
      {
        progScanProjects();
      }
      app.dirty = true;
    }
  }
  else if (app.screen == INFO)
  {
    if (pressed & (1U << BTN_M))
    {
      app.screen = MENU;
      app.dirty  = true;
    }
  }
  else if (app.screen == FILES)
  {
    if (pressed & (1U << BTN_M))
    {
      app.screen = MENU;
      app.dirty  = true;
    }
    else if (app.mounted && app.count)
    {
      if (pressed & (1U << BTN_UP))
      {
        app.selected = (app.selected + app.count - 1) % app.count;
        app.dirty    = true;
      }
      if (pressed & (1U << BTN_DN))
      {
        app.selected = (app.selected + 1) % app.count;
        app.dirty    = true;
      }
      if (pressed & (1U << BTN_SEL))
      {
        app.stage    = "OPEN PROJECT";
        app.error[0] = 0;
        memset(&app.result, 0, sizeof(app.result));
        bool ok = progLoadManifest(app.ids[app.selected]);
        if (ok && !app.esp_project)
        {
          char    path[128];
          FILINFO info;
          snprintf(path, sizeof(path), "/prog/fw/%s/%s", app.ids[app.selected], app.manifest.image);
          FRESULT e = f_stat(path, &info);
          if (e != FR_OK)
          {
            ok = progIoFail(path, e);
          }
          else if ((info.fattrib & AM_DIR) || info.fsize < 8 || info.fsize > 0x100000)
          {
            ok = progFail("Invalid image size");
          }
          else
          {
            app.image_size = (uint32_t)info.fsize;
          }
        }
        app.screen        = ok ? CONFIRM : RESULT;
        app.confirm_armed = false;
        app.hold_start    = 0;
        app.dirty         = true;
      }
    }
  }
  else if (app.screen == AUTO_SETUP)
  {
    if (pressed & (1U << BTN_M))
    {
      app.screen = MENU;
    }
    else if (pressed & (1U << BTN_UP))
    {
      app.auto_selected = (app.auto_selected + PROG_AUTO_TARGETS - 1) % PROG_AUTO_TARGETS;
    }
    else if (pressed & (1U << BTN_DN))
    {
      app.auto_selected = (app.auto_selected + 1) % PROG_AUTO_TARGETS;
    }
    else if (pressed & (1U << BTN_SEL))
    {
      app.auto_target = app.auto_selected;
      progAutoProjects();
      app.screen = AUTO_FILES;
    }
    app.dirty |= pressed != 0;
  }
  else if (app.screen == AUTO_FILES)
  {
    if (pressed & (1U << BTN_M))
    {
      app.screen = AUTO_SETUP;
    }
    else if (app.mounted)
    {
      if (pressed & (1U << BTN_UP))
      {
        app.selected = (app.selected + app.count) % (app.count + 1);
      }
      else if (pressed & (1U << BTN_DN))
      {
        app.selected = (app.selected + 1) % (app.count + 1);
      }
      else if (pressed & (1U << BTN_SEL))
      {
        if (progAutoSave(app.selected ? app.ids[app.selected - 1] : ""))
        {
          app.screen = AUTO_SETUP;
          buzzerBeep(60);
        }
        else
        {
          snprintf(app.error, sizeof(app.error), "Save failed");
        }
      }
    }
    app.dirty |= pressed != 0 || (uint32_t)(millis() - app.draw_time) >= 400;
  }
  else if (app.screen == ERASE_TARGET)
  {
    if (pressed & (1U << BTN_M))
    {
      app.screen = MENU;
      app.dirty  = true;
    }
    else
    {
      if (pressed & (1U << BTN_UP))
      {
        app.erase_target = (app.erase_target + 3) % 4;
        app.dirty        = true;
      }
      if (pressed & (1U << BTN_DN))
      {
        app.erase_target = (app.erase_target + 1) % 4;
        app.dirty        = true;
      }
      if (pressed & (1U << BTN_SEL))
      {
        app.screen        = ERASE_CONFIRM;
        app.confirm_armed = false;
        app.hold_start    = 0;
        app.dirty         = true;
      }
    }
  }
  else if (app.screen == CONFIRM || app.screen == ERASE_CONFIRM)
  {
    if (pressed & (1U << BTN_M))
    {
      app.screen = app.erase_job ? ERASE_TARGET : FILES;
      app.dirty  = true;
    }
    else if (!app.mounted && !progIsEspErase())
    {
      app.confirm_armed = false;
      app.hold_start    = 0;
    }
    else if (!(buttons & (1U << BTN_SEL)))
    {
      app.confirm_armed = true;
      app.hold_start    = 0;
    }
    else if (app.confirm_armed)
    {
      if (!app.hold_start)
      {
        app.hold_start = millis();
      }
      if ((uint32_t)(millis() - app.hold_start) >= 1000)
      {
        progRunJob();
      }
    }
  }
  else if (app.screen == RESULT && (pressed & ((1U << BTN_SEL) | (1U << BTN_M))))
  {
    if (app.auto_job)
    {
      if ((pressed & (1U << BTN_M)) || !app.error[0]) /* PASS returns home; the next unit starts from there. */
      {
        app.auto_job = false;
        app.screen   = HOME;
        app.dirty    = true;
      }
      else
      {
        progRunAuto();
      }
    }
    else
    {
      app.screen = app.erase_job ? MENU : FILES;
      progScanProjects();
    }
  }
  if ((uint32_t)(millis() - app.draw_time) >= FRAME_MS &&
      (progAnim().active || (app.sel_screen == app.screen && (uint32_t)(millis() - app.sel_start) < SLIDE_MS + FRAME_MS)))
  {
    app.dirty = true;
  }
  progDraw();
}

static void cliProg(cli_args_t *args)
{
  if (args->argc == 1 && args->isStr(0, "list"))
  {
    progScanProjects();
    if (app.screen == AUTO_FILES)
    {
      app.screen = AUTO_SETUP;
    }
    if (app.screen == CONFIRM || app.screen == ERASE_CONFIRM || app.screen == RESULT)
    {
      app.screen        = HOME;
      app.confirm_armed = false;
    }
    for (unsigned i = 0; i < app.count; i++)
    {
      cliPrintf("%s\n", app.ids[i]);
    }
    cliPrintf("%u projects%s\n", app.count, app.more ? " (limit 16; more omitted)" : "");
    return;
  }
  if (args->argc == 2 && args->isStr(0, "check"))
  {
    cliPrintf("SD file check; no target access.\n");
    bool ok = progPreflight(args->getStr(1));
    if (!progCloseImage())
    {
      ok = false;
    }
    cliPrintf("CHECK    : %s (SD files only; target flash not modified)\n", ok ? "OK" : "FAILED");
    app.auto_job  = false;
    app.erase_job = false;
    app.screen    = FILES;
    app.dirty     = true;
    return;
  }
  cliPrintf("prog list\nprog check <project>\nUse OLED: UP/DN, SEL, then hold SEL 1s to write.\n");
}

static void cliEsp(cli_args_t *args)
{
  if (args->argc == 1 && args->isStr(0, "info"))
  {
    cliPrintf("ESP five-pin: CLK/PB4=TX, SWO/PC1=RX, nRESET/PB3=EN, DIO/PB1=BOOT, GND\n"
              "Connect at 115200 8N1; Flash/Erase link test up to 921600. Target powered separately.\n"
              "ESP32/S3 BOOT=GPIO0; C3 BOOT=GPIO9. CLI USART1 remains available.\n");
    return;
  }
  if (args->argc == 1 && args->isStr(0, "probe"))
  {
    cliPrintf("ESP probe: resets target into ROM downloader; no erase/write.\n");
    esp_program_result_t result;
    espProgramProbe(&result);
    progEspReport(&result);
    return;
  }
  cliPrintf("esp info\nesp probe (resets target, no flash write)\n");
}

static bool progInit(void)
{
  app.dirty         = true;
  app.stage         = "IDLE";
  app.last_buttons  = buttonGetData();
  app.screen        = HOME;
  app.menu_selected = 0;
  app.detected      = sdIsDetected();
  app.mounted       = app.detected && fatfsIsMount();
  progScanProjects();
  progAutoLoad();
  bool ok = cliAdd("prog", cliProg);
  return cliAdd("esp", cliEsp) && ok;
}

MODULE_DEF(prog)
{
  .name     = "prog",
  .priority = MODULE_PRI_HIGH,
  .init     = progInit,
};
