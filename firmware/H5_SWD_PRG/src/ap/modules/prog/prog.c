#include "module.h"
#include "fatfs.h"
#include "prog.h"
#include "prog_catalog.h"
#include "prog_storage.h"
#include "esp_program.h"
#include "prog_auto.h"
#include "prog_keyboard.h"
#include "prog_power.h"
#include "net/net.h"
#include "net/hub.h"
#include "swd/swd_program.h"
#include "lcd/lcd_fonts.h"
#include <math.h>
#include <string.h>

#define LOADER_PATH "/prog/loaders/st/0x483.stldr"
#define LOADER_CRC  0x32674EA9U
#define FRAME_MS    25 /* ~30 fps; one I2C OLED frame takes about 25 ms. */
#define SLIDE_MS    140
#define RUN_FRAME_MS 50 /* Progress screen; mostly the bar's two pages change, about 6 ms of I2C each. */
#define SCROLL_MS   400 /* A selected label that does not fit scrolls one character per step. */
#define BAR_W       116 /* Inside of the progress capsule. */
#define PILL_W      104
#define PILL_H      32
#define PILL_R      7
#define REPEAT_MS   400 /* Held UP/DN on the keyboard: first repeat, then every REPEAT_STEP_MS. */
#define REPEAT_STEP_MS 70
#define MENU_HOLD_MS 600 /* Held SEL opens the keyboard menu. */
#define NET_POLL_MS 1000
#define SCAN_MS     15000
#define WIFI_JOIN_MS 30000
#define LTE_JOIN_MS 240000 /* Modem boot, registration and PDP can take minutes on a cold SIM. */
#define POWER_SETTLE_MS 250 /* Target supply switched on by a job: rails and the target's reset settle first. */

_Static_assert(PROG_AUTO_TARGETS == PROG_TARGETS, "auto*.cfg holds one project per prog_target_t");
_Static_assert(PROG_AUTO_ID_SIZE == PROG_ID_SIZE, "Auto bindings store catalog IDs");

typedef enum
{
  HOME, /* Auto dashboard; the production entry point. */
  MENU,
  MANUAL,
  STORAGE,
  POWER, /* Target supply outputs. */
  INFO,
  CHIPS,   /* One row per chip; app.pick decides where SEL leads. */
  PACKAGES, /* SD packages of app.target. */
  DETAILS,  /* One installed package. */
  NETWORK,
  CONNECT,     /* Wi-Fi, LTE or off. */
  WIFI_LIST,   /* Scan result, strongest first. */
  KEYBOARD,    /* Wi-Fi password. */
  APN_LIST,
  NET_STATUS,
  HUB_LIST,    /* Hub projects of app.target. */
  HUB_CONFIRM,
  CONFIRM,
  ERASE_CONFIRM,
  WIPE_CONFIRM,
  RUNNING,
  RESULT
} screen_t;

/* What a chip list was opened for. Every package list is grouped by the chip its fw.txt names. */
typedef enum
{
  PICK_AUTO,     /* Bind one package per chip for Auto. */
  PICK_FLASH,    /* Manual write. */
  PICK_ERASE,    /* Manual full-chip erase; no package. */
  PICK_INSTALLED, /* Storage view, including packages that cannot be written. */
  PICK_HUB        /* Hub projects to install, grouped by the chip the Hub names. */
} pick_t;

typedef enum
{
  JOB_FLASH,
  JOB_ERASE,
  JOB_WIPE,
  JOB_SCAN, /* Network jobs from here on. */
  JOB_JOIN,
  JOB_NET_OFF,
  JOB_HUB_LIST,
  JOB_INSTALL
} job_t;

typedef struct
{
  FIL     file;
  FRESULT error;
} sd_file_t;

/* One list row: the label scrolls when it is selected and too long. */
typedef struct
{
  const char     *text;
  char            right[12];
  const uint16_t *icon; /* Menu rows. */
  int8_t          bars; /* Wi-Fi signal 0..4, or -1. */
} row_t;

static struct
{
  screen_t              screen;
  pick_t                pick;
  job_t                 job;
  bool                  auto_job;
  unsigned              menu_selected;
  unsigned              manual_selected;
  unsigned              storage_selected;
  unsigned              network_selected;
  unsigned              power_selected;
  prog_power_config_t   power;
  bool                  power_on;    /* An enabled output is currently switched on. */
  bool                  power_saved; /* The settings on screen are the ones on the card. */
  unsigned              connect_selected;
  unsigned              apn_selected;
  unsigned              ap_selected;  /* WIFI_LIST row; the last row rescans. */
  unsigned              hub_selected; /* HUB_LIST row. */
  net_ap_t              aps[NET_AP_MAX];
  unsigned              ap_count;
  net_info_t            net_info;
  uint32_t              net_poll;
  uint8_t               join_mode; /* net_mode_t of JOB_JOIN. */
  char                  join_ssid[NET_SSID_SIZE];
  char                  join_secret[NET_PASSWORD_SIZE]; /* Wi-Fi password or LTE APN. */
  kbd_t                 kbd;
  uint32_t              repeat_at; /* Next UP/DN auto-repeat on the keyboard. */
  uint32_t              sel_down;
  bool                  sel_used; /* The current SEL press already acted (menu, or it opened the keyboard). */
  hub_list_t            hub;
  uint8_t               hub_view[HUB_PROJECTS];
  unsigned              hub_view_count;
  unsigned              target;   /* prog_target_t: chip row, erase chip, and the chip Auto detected. */
  unsigned              selected; /* Package row; in PICK_AUTO row 0 clears the binding. */
  prog_catalog_t        catalog;
  FRESULT               catalog_error;
  uint8_t               view[PROG_CATALOG_SIZE]; /* Catalog indices of app.target for app.pick. */
  unsigned              view_count;
  char                  job_id[PROG_ID_SIZE];
  int                   auto_slot;
  prog_auto_config_t    auto_config;
  bool                  loader_present;
  bool                  mounted;
  bool                  detected;
  bool                  dirty;
  bool                  marquee; /* The last frame scrolled a label. */
  bool                  confirm_armed;
  bool                  cancel;
  bool                  image_open;
  uint32_t              last_buttons;
  uint32_t              hold_start;
  uint32_t              draw_time;
  uint32_t              anim_key; /* Screen state the running animation belongs to. */
  uint32_t              anim_start;
  int                   sel_row; /* Selection bar target for this frame, or -1. */
  int                   sel_from;
  int                   sel_to;
  uint32_t              sel_start;
  screen_t              sel_screen;
  float                 bar_px; /* Eased progress fill, px. */
  float                 bar_v;  /* Jelly bar spring velocity, px/s. */
  uint32_t              bar_time;
  char                  stage_name[24]; /* Install: file shown, slides in when it changes. */
  uint32_t              stage_at;
  unsigned              pct_seen; /* Install: percent shown, pops when it changes. */
  uint32_t              pct_at;
  uint32_t              job_start;
  uint32_t              done;
  uint32_t              total;
  const char           *stage;
  char                  error[96];
  unsigned              esp_file_index;
  esp_program_result_t  esp_result;
  prog_package_t        package;
  prog_storage_result_t wipe;
  prog_loader_t         loader;
  prog_image_t          image;
  swd_program_result_t  result;
  sd_file_t             image_file;
  uint32_t              image_size;
  uint32_t              code[1024];
} app;

static void progDraw(void);
static void progOpen(screen_t screen);
static void progOpenChips(pick_t pick);

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
  memset(next.project[app.target], 0, PROG_AUTO_ID_SIZE);
  snprintf(next.project[app.target], PROG_AUTO_ID_SIZE, "%s", id);
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

static bool progAutoBound(const prog_catalog_entry_t *entry)
{
  return entry->target < PROG_TARGETS && !strcmp(app.auto_config.project[entry->target], entry->id);
}

/* Drives 3V_OUT/5V_OUT: the enabled outputs while on, both off otherwise. */
static void progPowerOutput(bool on)
{
  gpioPinWrite(TARGET_3V3, on && app.power.out_3v3);
  gpioPinWrite(TARGET_5V, on && app.power.out_5v);
  app.power_on = on && (app.power.out_3v3 || app.power.out_5v);
}

/* Between jobs: Always keeps the enabled outputs on, Job leaves the target unpowered. */
static void progPowerIdle(void)
{
  progPowerOutput(app.power.mode == PROG_POWER_ALWAYS);
}

/* Before the first target access of a job, so SWD or the ESP ROM is probed on a target that has started. */
static void progPowerJob(void)
{
  if (app.power_on || (!app.power.out_3v3 && !app.power.out_5v))
  {
    return;
  }
  const char *stage = app.stage;
  app.stage         = "POWER ON";
  app.dirty         = true;
  progDraw();
  progPowerOutput(true);
  cliPrintf("POWER    : 3V3=%s 5V=%s for this job\n", app.power.out_3v3 ? "ON" : "OFF", app.power.out_5v ? "ON" : "OFF");
  delay(POWER_SETTLE_MS);
  app.stage = stage;
}

/* A new card brings its own settings; a pulled card leaves the outputs as they are. */
static void progPowerReload(void)
{
  if (!progPowerLoad(&app.power))
  {
    memset(&app.power, 0, sizeof(app.power));
  }
  app.power_saved = true;
  progPowerIdle();
}

static void progPowerChange(void)
{
  app.power_saved = app.mounted && sdIsDetected() && progPowerSave(&app.power);
  progPowerIdle();
  app.dirty = true;
}

static bool progIsEspErase(void)
{
  const prog_target_info_t *info = progTargetInfo((prog_target_t)app.target);
  return app.job == JOB_ERASE && info && info->esp;
}

/* Chip rows; the Storage view adds "Unknown" when a package names no supported device. */
static unsigned progChipRows(void)
{
  if (app.pick == PICK_HUB)
  {
    return PROG_TARGETS; /* Hub projects for chips this firmware cannot write are not offered. */
  }
  bool unknown = app.pick == PICK_INSTALLED && progCatalogFilter(&app.catalog, PROG_TARGET_NONE, false, NULL, 0);
  return PROG_TARGETS + (unknown ? 1 : 0);
}

static bool progUsableOnly(void)
{
  return app.pick != PICK_INSTALLED;
}

static unsigned progPackageRows(void)
{
  return app.view_count + (app.pick == PICK_AUTO ? 1 : 0);
}

/* NULL for the "(None)" row of the Auto list. */
static const prog_catalog_entry_t *progPackageAt(unsigned row)
{
  if (app.pick == PICK_AUTO)
  {
    if (!row)
    {
      return NULL;
    }
    row--;
  }
  return row < app.view_count ? &app.catalog.entries[app.view[row]] : NULL;
}

static void progViewBuild(void)
{
  app.view_count =
  progCatalogFilter(&app.catalog, (prog_target_t)app.target, progUsableOnly(), app.view, PROG_CATALOG_SIZE);
  unsigned rows = progPackageRows();
  if (app.selected >= rows)
  {
    app.selected = rows ? rows - 1 : 0;
  }
  if (app.screen == DETAILS && !progPackageAt(app.selected))
  {
    app.screen = PACKAGES;
  }
}

/* Re-reads every fw.txt; called when the card changes and after jobs that touch it. */
static void progRefresh(void)
{
  memset(&app.catalog, 0, sizeof(app.catalog));
  app.catalog_error  = FR_OK;
  app.loader_present = false;
  app.dirty          = true;
  if (app.mounted)
  {
    FRESULT e = progCatalogScan(&app.catalog);
    if (e != FR_OK && e != FR_NO_FILE && e != FR_NO_PATH) /* A card without /prog/fw is simply empty. */
    {
      app.catalog_error = e;
    }
    FILINFO info;
    app.loader_present = f_stat(LOADER_PATH, &info) == FR_OK;
  }
  if (app.target >= progChipRows())
  {
    app.target = 0;
  }
  progViewBuild();
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
  FRESULT     io;
  const char *error = progPackageLoad(id, &app.package, &io);
  if (io != FR_OK)
  {
    char path[64];
    snprintf(path, sizeof(path), "/prog/fw/%s/fw.txt", id);
    return progIoFail(path, io);
  }
  if (error)
  {
    return progFail(error);
  }
  app.image_size = app.package.esp_project ? app.package.esp.total : 0;
  return true;
}

static bool progProgress(void *context, const char *step, uint32_t done, uint32_t total)
{
  (void)context;
  app.stage = step;
  app.done  = done;
  app.total = total;
  bool sd = app.job == JOB_FLASH || app.job == JOB_WIPE || app.job == JOB_INSTALL || (app.job == JOB_ERASE && !progIsEspErase());
  if (buttonGetPressed(BTN_M) || (sd && !sdIsDetected()))
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
  cliPrintf("TARGET   : %s, %u images, baud ceiling=%lu%s (five-pin UART)\n", espProjectChipName(app.package.esp.chip),
            app.package.esp.count, (unsigned long)(app.package.esp.baud ? app.package.esp.baud : 921600), app.package.esp.baud ? "" : " auto");
  for (unsigned i = 0; i < app.package.esp.count; i++)
  {
    const esp_project_image_t *img = &app.package.esp.images[i];
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
      else if (header[0] == 0xE9 && (header[12] | ((uint16_t)header[13] << 8)) != espProjectChipId(app.package.esp.chip))
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
  app.image_size     = app.package.esp.total;
  app.esp_file_index = UINT32_MAX;
  return true;
}

static bool progReadEspImage(void *context, unsigned image, uint32_t offset, void *data, uint32_t size)
{
  (void)context;
  if (image >= app.package.esp.count || offset > app.package.esp.images[image].size || size > app.package.esp.images[image].size - offset ||
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
    snprintf(path, sizeof(path), "/prog/fw/%s/%s", app.job_id, app.package.esp.images[image].file);
    if (!progOpenFile(&app.image_file, path, &file))
    {
      return false;
    }
    app.image_open     = true;
    app.esp_file_index = image;
    if (file.size != app.package.esp.images[image].size)
    {
      progCloseImage();
      return progFail("ESP SD size changed");
    }
  }
  if (!progReadFile(&app.image_file, offset, data, size))
  {
    return progIoFail("ESP image read", app.image_file.error);
  }
  return offset + size == app.package.esp.images[image].size ? progCloseImage() : true;
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
  cliPrintf("PROJECT  : %s\n", app.package.manifest.name);
  if (app.package.esp_project)
  {
    return progEspPreflight(id);
  }
  if (!progLoadLoader(app.package.manifest.algo, app.package.manifest.loader_crc32))
  {
    return false;
  }
  prog_file_t f;
  uint32_t    crc   = 0;
  const char *error = NULL;
  char        path[128];
  snprintf(path, sizeof(path), "/prog/fw/%s/%s", id, app.package.manifest.image);
  if (!progOpenFile(&app.image_file, path, &f))
  {
    return false;
  }
  app.image_open = true;
  app.image_size = f.size;
  error          = progInspectImage(&f, app.package.manifest.address, &app.image);
  if (!error && !progFileCrc32(&f, &crc))
  {
    error = "Image read failed";
  }
  if (!error && crc != app.package.manifest.crc32)
  {
    error = "Image CRC mismatch";
  }
  if (error)
  {
    progCloseImage();
    return progFail(error);
  }
  cliPrintf("IMAGE    : %s, %lu bytes, CRC32=%08lX OK\n", path, (unsigned long)f.size, (unsigned long)crc);
  cliPrintf("ADDRESS  : %08lX SP=%08lX Reset=%08lX\n", (unsigned long)app.package.manifest.address, (unsigned long)app.image.sp,
            (unsigned long)app.image.reset);
  return true;
}

static void progFinish(bool ok)
{
  progPowerIdle();
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

static bool progWipe(void)
{
  if (!app.mounted || !sdIsDetected() || !fatfsIsMount())
  {
    return progFail("SD not mounted");
  }
  const prog_storage_input_t input = {NULL, progProgress};
  const char                *error = progStorageWipe(&input, &app.wipe);
  cliPrintf("SD DELETE: %s, %lu entries removed\n", error ? error : "OK", (unsigned long)app.wipe.removed);
  if (app.wipe.io != FR_OK)
  {
    return progIoFail("SD delete", app.wipe.io);
  }
  return error ? progFail(error) : true;
}

/* Wi-Fi scan: the ESP scans in the background; the list is ready when it stops answering BUSY. */
static bool progNetScan(void)
{
  uint8_t status = netWifiScan();
  if (status != NET_ST_OK)
  {
    return progFail(netStatusText(status));
  }
  uint32_t start = millis(), poll = 0;
  while (progProgress(NULL, "WIFI SCAN", 0, 1))
  {
    if ((uint32_t)(millis() - poll) < 300)
    {
      continue;
    }
    poll   = millis();
    status = netWifiList(app.aps, NET_AP_MAX, &app.ap_count);
    if (status == NET_ST_OK)
    {
      return true;
    }
    if (status != NET_ST_BUSY)
    {
      return progFail(netStatusText(status));
    }
    if ((uint32_t)(millis() - start) >= SCAN_MS)
    {
      return progFail("Scan timeout");
    }
  }
  return progFail("Stopped");
}

/* Join and wait until the ESP reports this network connected; settings are saved only after that. */
static bool progNetJoin(void)
{
  bool        wifi   = app.join_mode == NET_MODE_WIFI;
  const char *stage  = wifi ? "WIFI JOIN" : "LTE ATTACH";
  uint8_t     status = wifi ? netWifiJoin(app.join_ssid, app.join_secret) : netLteJoin(app.join_secret);
  cliPrintf("JOIN     : %s %s -> %s\n", wifi ? "Wi-Fi" : "LTE", wifi ? app.join_ssid : app.join_secret,
            netStatusText(status));
  if (status != NET_ST_OK)
  {
    return progFail(netStatusText(status));
  }
  uint32_t start = millis(), poll = 0, limit = wifi ? WIFI_JOIN_MS : LTE_JOIN_MS;
  while (progProgress(NULL, stage, 0, 1))
  {
    if ((uint32_t)(millis() - poll) < 500)
    {
      continue;
    }
    poll   = millis();
    status = netInfo(&app.net_info);
    if (status != NET_ST_OK)
    {
      return progFail(netStatusText(status));
    }
    const net_info_t *info = &app.net_info;
    bool              mine = info->mode == app.join_mode && (!wifi || !strcmp(info->detail, app.join_ssid));
    if (info->state == NET_STATE_CONNECTED && mine)
    {
      net_store_t *settings = netSettings();
      settings->mode        = app.join_mode;
      if (wifi)
      {
        netStoreRemember(settings, app.join_ssid, app.join_secret);
        snprintf(settings->ssid, sizeof(settings->ssid), "%s", app.join_ssid);
      }
      else
      {
        snprintf(settings->apn, sizeof(settings->apn), "%.63s", app.join_secret);
      }
      if (!netSettingsSave())
      {
        cliPrintf("JOIN     : connected; settings not saved (SD)\n");
      }
      return true;
    }
    /* A failure left over from the previous attempt can show for a moment after the request. */
    if (info->state == NET_STATE_FAILED && (uint32_t)(millis() - start) > 1000)
    {
      return progFail(info->detail[0] ? info->detail : "Join failed");
    }
    if ((uint32_t)(millis() - start) >= limit)
    {
      netOff();
      return progFail("Join timeout");
    }
  }
  netOff();
  return progFail("Stopped");
}

static const hub_project_t *progHubAt(unsigned row)
{
  return row < app.hub_view_count ? &app.hub.projects[app.hub_view[row]] : NULL;
}

/* The SD package with the same ID, if any. */
static const prog_catalog_entry_t *progHubInstalled(const hub_project_t *project)
{
  for (unsigned i = 0; project && i < app.catalog.count; i++)
  {
    if (!strcmp(app.catalog.entries[i].id, project->id))
    {
      return &app.catalog.entries[i];
    }
  }
  return NULL;
}

static unsigned progHubCount(prog_target_t target, uint8_t *view)
{
  unsigned count = 0;
  for (unsigned i = 0; i < app.hub.count; i++)
  {
    if (app.hub.projects[i].target == target)
    {
      if (view)
      {
        view[count] = (uint8_t)i;
      }
      count++;
    }
  }
  return count;
}

/* Why Install cannot start, or NULL. The ST loader is not on the Hub, so a missing one is reported here. */
static const char *progHubBlocked(const hub_project_t *project)
{
  const prog_target_info_t *info = progTargetInfo(project->target);
  if (!app.mounted)
  {
    return "No SD card";
  }
  if (!netIsConnected())
  {
    return "Not connected";
  }
  if (info && !info->esp && !app.loader_present)
  {
    return "No ST loader!";
  }
  if (!progHubInstalled(project) && app.catalog.count >= PROG_CATALOG_SIZE)
  {
    return "SD full (16)";
  }
  return NULL;
}

static void progRunNetJob(void)
{
  bool        ok    = false;
  const char *error = NULL;
  switch (app.job)
  {
    case JOB_SCAN:
      if (progNetScan())
      {
        ledActivityStop();
        app.last_buttons = buttonGetData(); /* A key held through the job does not act on the next screen. */
        app.ap_selected  = 0;
        progOpen(WIFI_LIST);
        return;
      }
      break;
    case JOB_JOIN:
      ok = progNetJoin();
      break;
    case JOB_HUB_LIST:
      app.net_info = *netInfoCached();
      if (!netIsConnected())
      {
        progFail(netIsPresent() ? "Not connected" : "No ESP link");
        break;
      }
      error = hubFetchList(&app.hub, progProgress, NULL);
      cliPrintf("HUB      : %s, %u projects%s\n", error ? error : "OK", app.hub.count, app.hub.more ? "+" : "");
      if (!error)
      {
        ledActivityStop();
        app.last_buttons = buttonGetData();
        progOpenChips(PICK_HUB);
        return;
      }
      progFail(error);
      break;
    case JOB_INSTALL:
    {
      const hub_project_t *project = progHubAt(app.hub_selected);
      error                        = project ? progHubBlocked(project) : "No project";
      if (!error)
      {
        error = hubInstall(project, progProgress, NULL);
        cliPrintf("INSTALL  : %s %s -> %s\n", project->id, project->version, error ? error : "OK");
      }
      ok = !error;
      if (error)
      {
        progFail(error);
      }
      progRefresh();
      break;
    }
    default:
      break;
  }
  progFinish(ok);
}

static void progRunJob(void)
{
  app.screen    = RUNNING;
  app.stage     = progIsEspErase() ? "ESP PREFLIGHT" : app.job == JOB_WIPE ? "SCAN SD" : "CHECK SD FILES";
  app.cancel    = false;
  app.done      = 0;
  app.total     = 1;
  app.job_start = millis();
  memset(&app.result, 0, sizeof(app.result));
  app.stage_name[0] = 0;
  app.pct_seen      = 0;
  app.bar_v         = 0;
  app.dirty         = true;
  progDraw();
  app.error[0] = 0;
  if (app.job >= JOB_SCAN)
  {
    progRunNetJob();
    return;
  }
  if (app.job == JOB_WIPE)
  {
    bool ok = progWipe();
    progAutoLoad(); /* The Auto bindings were on the card too. */
    progRefresh();
    progFinish(ok);
    return;
  }
  if (app.job == JOB_ERASE)
  {
    cliPrintf("ERASE    : %s, entire flash including application and stored data\n",
              progTargetLabel((prog_target_t)app.target));
  }
  bool ok = progIsEspErase()       ? true
            : app.job == JOB_ERASE ? progLoadLoader(LOADER_PATH, LOADER_CRC)
                                   : progPreflight(app.job_id);
  if (ok && app.auto_job && app.package.target != app.target)
  {
    ok = progFail("Auto chip mismatch");
  }
  if (ok && !progProgress(NULL, app.stage, 1, 1))
  {
    ok = progFail("Stopped before reset");
  }
  if (ok)
  {
    progPowerJob();
  }
  if (ok && (progIsEspErase() || (app.job == JOB_FLASH && app.package.esp_project)))
  {
    const esp_program_input_t input = {NULL, progIsEspErase() ? NULL : progReadEspImage, progProgress};
    ok = progIsEspErase() ? espProgramErase(progTargetInfo((prog_target_t)app.target)->esp_chip, &input, &app.esp_result)
                          : espProgramRun(&app.package.esp, &input, &app.esp_result);
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
                                 app.job == JOB_ERASE ? 0 : app.image_size,
                                 app.job == JOB_ERASE ? 0 : app.package.manifest.crc32,
                                 NULL,
                                 app.job == JOB_ERASE ? NULL : progReadImage,
                                 progProgress};
    swd_err_t           e     = app.job == JOB_ERASE ? swdEraseH723(&input, &app.result) : swdProgramH723(&input, &app.result);
    ok                        = (e == SWD_OK);
    if (!ok && !app.error[0])
    {
      progFail(app.result.cancelled ? "Stopped" : swdErrStr(e));
    }
    cliPrintf("%s : %s at %s, address=%08lX, SR=%08lX\n", app.job == JOB_ERASE ? "ERASE   " : "PROGRAM ", swdErrStr(e),
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
  app.job       = JOB_FLASH;
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
    progPowerJob(); /* Stays on through the job that follows detection. */
    swd_probe_t probe;
    swd_err_t   status = swdProbeH723(100, &probe);
    if (status == SWD_OK)
    {
      app.target = PROG_TARGET_H723;
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
          app.target = chip == ESP32_CHIP     ? PROG_TARGET_ESP32
                       : chip == ESP32S3_CHIP ? PROG_TARGET_ESP32S3
                                              : PROG_TARGET_ESP32C3;
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
      const char *id = app.auto_config.project[app.target];
      cliPrintf("AUTO     : %s -> %s\n", progTargetLabel((prog_target_t)app.target), id[0] ? id : "(not assigned)");
      if (!id[0])
      {
        progFail("Set Auto project");
      }
      else
      {
        /* Use the saved ID directly; a change in directory sort order cannot select another package. */
        strcpy(app.job_id, id);
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
static const uint16_t icon_list[12]  = {0, 0xDFF, 0xDFF, 0, 0, 0xDFF, 0xDFF, 0, 0, 0xDFF, 0xDFF, 0};
static const uint16_t icon_wifi[12]  = {0, 0x3FC, 0xC03, 0, 0x0F0, 0x30C, 0, 0, 0x060, 0x060, 0, 0};
static const uint16_t icon_power[12] = {0x060, 0x264, 0x462, 0x861, 0x861, 0x801,
                                        0x801, 0x801, 0x402, 0x204, 0x1F8, 0};
static const uint16_t icon_get[12]   = {0x060, 0x060, 0x060, 0x060, 0x666, 0x36C, 0x1F8, 0x0F0, 0x060, 0, 0xFFF, 0xFFF};

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

/* 7-px proportional glyphs for the power badge; bit (width - 1) is the leftmost column. '!' is the bolt. */
typedef struct
{
  char    c;
  uint8_t width;
  uint8_t rows[7];
} mini_glyph_t;

static const mini_glyph_t mini_glyphs[] = {
  {'!', 5, {0x03, 0x06, 0x0C, 0x1F, 0x06, 0x0C, 0x18}},
  {'3', 4, {0x0E, 0x01, 0x01, 0x06, 0x01, 0x01, 0x0E}},
  {'5', 4, {0x0F, 0x08, 0x0E, 0x01, 0x01, 0x09, 0x06}},
  {'.', 1, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
  {'V', 5, {0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04, 0x04}},
  {'&', 5, {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}},
};

static const mini_glyph_t *progMiniGlyph(char c)
{
  for (unsigned i = 0; i < sizeof(mini_glyphs) / sizeof(mini_glyphs[0]); i++)
  {
    if (mini_glyphs[i].c == c)
    {
      return &mini_glyphs[i];
    }
  }
  return NULL;
}

/* Glyphs sit 1 px apart; the bolt and '&' get 2 px so the voltages read as separate words. Returns the width. */
static int progDrawMiniText(int x, int y, const char *text, uint16_t color, bool draw)
{
  int start = x;
  for (const char *p = text; *p; p++)
  {
    const mini_glyph_t *g = progMiniGlyph(*p);
    if (!g)
    {
      continue;
    }
    for (unsigned r = 0; draw && r < 7; r++)
    {
      for (unsigned col = 0; col < g->width; col++)
      {
        if (g->rows[r] & (1U << (g->width - 1 - col)))
        {
          lcdDrawPixel(x + col, y + r, color);
        }
      }
    }
    x += g->width;
    if (p[1])
    {
      x += (*p == '!' || *p == '&' || p[1] == '&') ? 2 : 1;
    }
  }
  return x - start;
}

/* Home shows which supply is live on the target connector right now. */
static void progDrawPowerBadge(void)
{
  if (!app.power_on)
  {
    return;
  }
  const char *text = app.power.out_3v3 && app.power.out_5v ? "!3.3V&5V" : app.power.out_3v3 ? "!3.3V" : "!5V";
  int         w    = progDrawMiniText(0, 0, text, black, false) + 6;
  lcdDrawFillRoundRect(0, 0, w, 11, 3, white);
  progDrawMiniText(3, 2, text, black, true);
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

/* Damped oscillation: amp at dt = 0, settling with the given decay and period (s). */
static float progSpring(float dt, float amp, float decay, float period)
{
  return dt < 0 ? 0 : amp * expf(-dt / decay) * cosf(6.2832f * dt / period);
}

static float progEaseOutBack(float q)
{
  float c1 = 1.70158f, c3 = c1 + 1.0f, u = q - 1.0f;
  return 1.0f + c3 * u * u * u + c1 * u * u;
}

static void progPixelColor(int x, int y, uint16_t color)
{
  if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT)
  {
    lcdDrawPixel(x, y, color);
  }
}

static void progPixel(int x, int y)
{
  progPixelColor(x, y, white);
}

/* Filled ellipse; a black one first gives the white one an outline against the stripes. */
static void progFillEllipse(float cx, float cy, float rx, float ry, uint16_t color)
{
  for (int dy = -(int)ry; dy <= (int)ry; dy++)
  {
    float half = rx * sqrtf(fmaxf(0.0f, 1.0f - (dy * dy) / (ry * ry)));
    for (int dx = -(int)half; dx <= (int)half; dx++)
    {
      progPixelColor((int)lroundf(cx) + dx, (int)lroundf(cy) + dy, color);
    }
  }
}

/* The 16x26 font scaled about its centre; scale 1 matches lcdPrintf. */
static void progDrawBigText(float cx, int y, const char *text, float scale)
{
  int   n    = (int)strlen(text);
  float w    = n * 16 * scale;
  float left = cx - w / 2.0f;
  float top  = y + 13.0f - 13.0f * scale;
  for (int j = 0; j < (int)(26 * scale + 1); j++)
  {
    int v = (int)(j / scale);
    if (v >= 26)
    {
      break;
    }
    for (int x = (int)ceilf(left); x <= (int)(left + w); x++)
    {
      int u = (int)((x - left) / scale);
      if (u >= 0 && u < n * 16 && ((font_16x26.data[(text[u / 16] - 32) * 26 + v] << (u % 16)) & 0x8000))
      {
        progPixel(x, (int)(top + j));
      }
    }
  }
}

/* Upward arc between 40 and 140 degrees; every'th sample only, for dotted or fading arcs. */
static void progDrawArc(int cx, int cy, float r, int every, bool thick)
{
  int n = (int)(r * 1.75f) + 4;
  for (int i = 0; i <= n; i += every)
  {
    float a = 0.698f + 1.745f * i / n;
    for (int k = 0; k <= (thick ? 1 : 0); k++)
    {
      progPixel(cx + (int)lroundf((r + k) * cosf(a)), cy - (int)lroundf((r + k) * sinf(a)));
    }
  }
}

/* Elapsed seconds at the top right; returns its left edge. */
static int progDrawElapsed(void)
{
  char          text[12];
  unsigned long seconds = (millis() - app.job_start) / 1000;
  snprintf(text, sizeof(text), "%lus", seconds > 99999 ? 99999 : seconds);
  int x = 126 - (int)strlen(text) * 7;
  lcdPrintf(x, 0, white, "%s", text);
  return x;
}

/* Wi-Fi scan and join: arcs light up from the centre with a springy overshoot, a ripple runs outwards,
 * and the dot squashes at the start of every wave. */
static void progDrawRadar(void)
{
  char title[24];
  if (app.job == JOB_SCAN)
  {
    snprintf(title, sizeof(title), "Wi-Fi scan");
  }
  else
  {
    snprintf(title, sizeof(title), "%s %.18s", app.join_mode == NET_MODE_LTE ? "LTE" : "Join",
             app.join_mode == NET_MODE_LTE ? app.join_secret : app.join_ssid);
  }
  int right = progDrawElapsed();
  lcdPrintf(2, 0, white, "%.*s", (right - 6) / 7, title);

  const float cycle = 1.4f;
  float       t     = (float)(millis() - app.job_start) / 1000.0f;
  float       p     = fmodf(t, cycle);
  int         cx = 64, cy = 47;
  float       s  = progSpring(p, 0.45f, 0.09f, 0.18f); /* Dot: squash (wide, low) then stretch. */
  int         dw = (int)lroundf(5 * (1 + s)), dh = (int)lroundf(5 * (1 - s));
  dw            = dw < 3 ? 3 : dw;
  dh            = dh < 2 ? 2 : dh;
  for (int y = 0; y < dh; y++)
  {
    for (int x = 0; x < dw; x++)
    {
      progPixel(cx - dw / 2 + x, cy + 2 - y);
    }
  }
  for (int i = 0; i < 3; i++)
  {
    float on = 0.12f + 0.18f * i; /* Inner arc first. */
    if (p < on)
    {
      continue;
    }
    float r = 9.0f + 7.5f * i + progSpring(p - on, 2.5f, 0.07f, 0.15f);
    progDrawArc(cx, cy, r, p > 1.05f ? 2 + (int)((p - 1.05f) * 10) : 1, p <= 1.05f);
  }
  if (p > 0.75f) /* Ripple fading as it spreads. */
  {
    float q = (p - 0.75f) / (cycle - 0.75f);
    progDrawArc(cx, cy, 27.0f + 6.0f * q, 2 + (int)(q * 5), false);
  }
  progDrawHint(app.cancel ? NULL : "M", app.cancel ? "Stopping..." : "stop", false);
}

/* Install and Hub list: the file name slides in, the percent pops when it changes, and the bar follows on a
 * spring with a jelly head that stretches with speed and wobbles when a new file starts. */
static void progDrawInstall(void)
{
  uint32_t now = millis();
  unsigned pct = app.total ? (unsigned)(((uint64_t)app.done * 100) / app.total) : 0;
  pct          = pct > 100 ? 100 : pct;
  float dt     = (float)(now - app.bar_time) / 1000.0f;
  if (strncmp(app.stage_name, app.stage, sizeof(app.stage_name) - 1))
  {
    snprintf(app.stage_name, sizeof(app.stage_name), "%s", app.stage);
    app.stage_at = now;
    app.bar_v -= 90.0f; /* A new file shakes the bar. */
  }
  if (pct != app.pct_seen)
  {
    app.pct_seen = pct;
    app.pct_at   = now;
  }

  int   right = progDrawElapsed();
  float q     = fminf(1.0f, (float)(now - app.stage_at) / 280.0f);
  int   x     = 2 + (int)lroundf(70.0f * (1.0f - progEaseOutBack(q)));
  int   room  = (right - 4 - x) / 7;
  if (room > 0)
  {
    lcdPrintf(x < 0 ? 0 : x, 0, white, "%.*s", room, app.stage_name);
  }

  char text[8];
  snprintf(text, sizeof(text), "%u%%", pct);
  float pop = 1.0f + progSpring((float)(now - app.pct_at) / 1000.0f, 0.2f, 0.08f, 0.2f);
  progDrawBigText(64.0f, 11, text, pop < 0.85f ? 0.85f : pop);

  /* Spring-damper towards the target, underdamped for a little overshoot. */
  const int y      = 39;
  float     target = pct * (float)BAR_W / 100.0f;
  if (dt > 0.5f || app.done == 0)
  {
    app.bar_px = target;
    app.bar_v  = 0;
    dt         = 0;
  }
  for (float step = fminf(dt, 0.1f); step > 0; step -= 0.01f) /* Fixed sub-steps keep the spring stable. */
  {
    float h = fminf(step, 0.01f);
    app.bar_v += (160.0f * (target - app.bar_px) - 10.0f * app.bar_v) * h;
    app.bar_px += app.bar_v * h;
  }
  app.bar_px   = fmaxf(0.0f, fminf((float)BAR_W, app.bar_px));
  app.bar_time = now;
  if (app.done == 0)
  {
    progDrawProgress(y, 0, true); /* Waiting for the first byte (TLS handshake). */
  }
  else
  {
    lcdDrawRoundRect(4, y, BAR_W + 8, 10, 4, white);
    float t     = (float)(now - app.job_start) / 1000.0f;
    int   march = (int)(t * 24.0f), width = (int)(app.bar_px + 0.5f);
    for (int bx = 0; bx < width; bx++)
    {
      for (int r = bx == 0 ? 1 : 0; r < (bx == 0 ? 5 : 6); r++)
      {
        if (((bx - r - march) % 8 + 8) % 8 < 6)
        {
          progPixel(8 + bx, y + 2 + r);
        }
      }
    }
    /* Jelly head: taller and thinner the faster it moves, bulging past the capsule, outlined in black. */
    float stretch = fminf(fabsf(app.bar_v) / 35.0f, 2.5f);
    float ry = 3.6f + stretch, rx = fmaxf(2.0f, 3.4f - stretch * 0.5f);
    float hx = 8.0f + app.bar_px, hy = y + 4.5f;
    progFillEllipse(hx, hy, rx + 1.0f, ry + 1.0f, black);
    progFillEllipse(hx, hy, rx, ry, white);
  }
  progDrawHint(app.cancel ? NULL : "M", app.cancel ? "Stopping..." : "stop", false);
}

/* Start of a label that is width characters wide on screen; a longer one pauses, scrolls, then pauses at its end. */
static const char *progScroll(const char *text, unsigned width)
{
  unsigned length = strlen(text);
  if (length <= width)
  {
    return text;
  }
  app.marquee     = true;
  unsigned offset = (millis() / SCROLL_MS) % (length - width + 6);
  offset          = offset < 3 ? 0 : offset - 3;
  return text + (offset > length - width ? length - width : offset);
}

static void progDrawNoCard(const char *hint)
{
  progDrawIcon(5, 21, icon_card, white, 2);
  lcdPrintf(39, 19, white, app.detected ? "SD not ready" : "No SD card");
  lcdPrintf(39, 34, white, "%s", hint ? hint : app.detected ? "Check FAT32" : "Insert card");
}

/* Header, up to three rows and the selection bar; menus (badge) show the SD state instead of the position. */
static void progDrawList(const char *title, unsigned count, unsigned selected, bool badge,
                         void (*fill)(unsigned item, row_t *row))
{
  progDrawHeader(title);
  if (badge)
  {
    progDrawSdBadge();
  }
  else
  {
    char position[12];
    snprintf(position, sizeof(position), "%u/%u", selected + 1, count);
    lcdPrintf(126 - (int)strlen(position) * 7, 0, white, "%s", position);
  }
  unsigned first = selected > 2 ? selected - 2 : 0;
  for (unsigned line = 0; line < 3 && first + line < count; line++)
  {
    unsigned item = first + line;
    bool     sel  = item == selected;
    int      y    = 15 + line * 12;
    row_t    row  = {"", "", NULL, -1};
    fill(item, &row);
    int x     = row.icon ? 22 : 12;
    int right = 121; /* Right edge of the bars and the right column. */
    if (row.bars >= 0)
    {
      for (int b = 0; b < 4; b++) /* Four bars, 2 px wide, rising to the right. */
      {
        int h = b < row.bars ? 2 + 2 * b : 1;
        lcdDrawFillRect(right - 11 + b * 3, y + 10 - h, 2, h, white);
      }
      right -= 14;
    }
    int end = row.right[0] ? right - 4 - (int)strlen(row.right) * 7 : row.icon ? 113 : right;
    int n   = (end - x) / 7;
    if (sel)
    {
      progDrawSelection(y, 12);
      lcdPrintf(row.icon ? 116 : 5, y + 1, white, ">");
    }
    if (row.icon)
    {
      progDrawIcon(5, y, row.icon, white, 1);
    }
    lcdPrintf(x, y + 1, white, "%.*s", n, sel ? progScroll(row.text, n) : row.text);
    if (row.right[0])
    {
      lcdPrintf(right - (int)strlen(row.right) * 7, y + 1, white, "%s", row.right);
    }
  }
  if (count > 3)
  {
    lcdDrawFillRect(126, 15, 1, 36, white);
    unsigned height = 36 * 3 / count;
    unsigned y      = 15 + (36 - height) * selected / (count - 1);
    lcdDrawFillRect(125, y, 3, height, white);
  }
}

static void progMenuRow(unsigned item, row_t *row)
{
  static const char     *items[] = {"Auto setup", "Manual", "Storage", "Network", "Power", "Info"};
  static const uint16_t *icons[] = {icon_setup, icon_flash, icon_card, icon_wifi, icon_power, icon_info};
  row->text                      = items[item];
  row->icon                      = icons[item];
}

static void progPowerRow(unsigned item, row_t *row)
{
  static const char *items[] = {"3.3V out", "5V out", "Supply"};
  row->text                  = items[item];
  if (item == 2)
  {
    strcpy(row->right, app.power.mode == PROG_POWER_ALWAYS ? "Always" : "Job");
  }
  else
  {
    strcpy(row->right, (item ? app.power.out_5v : app.power.out_3v3) ? "ON" : "OFF");
  }
}

static void progManualRow(unsigned item, row_t *row)
{
  row->text = item ? "Erase" : "Flash";
  row->icon = item ? icon_erase : icon_flash;
}

static void progStorageRow(unsigned item, row_t *row)
{
  row->text = item ? "SD Delete" : "Installed bin";
  row->icon = item ? icon_erase : icon_list;
}

static const char *progPickTitle(void)
{
  return app.pick == PICK_AUTO    ? "Auto setup"
         : app.pick == PICK_FLASH ? "Flash"
         : app.pick == PICK_ERASE ? "Erase"
         : app.pick == PICK_HUB   ? "Install"
                                  : "Installed";
}

static void progChipRow(unsigned item, row_t *row)
{
  prog_target_t target = item < PROG_TARGETS ? (prog_target_t)item : PROG_TARGET_NONE;
  row->text            = progTargetLabel(target);
  if (app.pick == PICK_AUTO)
  {
    const char *id = app.auto_config.project[target];
    snprintf(row->right, sizeof(row->right), "%.7s", id[0] ? id : "---");
  }
  else if (app.pick == PICK_HUB)
  {
    snprintf(row->right, sizeof(row->right), "%u", progHubCount(target, NULL));
  }
  else if (app.pick != PICK_ERASE)
  {
    snprintf(row->right, sizeof(row->right), "%u", progCatalogFilter(&app.catalog, target, progUsableOnly(), NULL, 0));
  }
}

static void progPackageRow(unsigned item, row_t *row)
{
  const prog_catalog_entry_t *entry = progPackageAt(item);
  row->text                         = entry ? entry->name : "(None)";
  if (entry && entry->error)
  {
    strcpy(row->right, "!"); /* Cannot be written; Details names the reason. */
  }
  else if (entry && progAutoBound(entry))
  {
    strcpy(row->right, "*"); /* Auto uses this package for its chip. */
  }
}

static void progNetworkRow(unsigned item, row_t *row)
{
  static const char     *items[] = {"Connect", "Install", "Status"};
  static const uint16_t *icons[] = {icon_wifi, icon_get, icon_info};
  row->text                      = items[item];
  row->icon                      = icons[item];
}

static void progConnectRow(unsigned item, row_t *row)
{
  static const char *items[] = {"Wi-Fi", "LTE", "Off"};
  row->text                  = items[item];
  if (item + 1 == netSettings()->mode || (item == 2 && netSettings()->mode == NET_MODE_OFF))
  {
    strcpy(row->right, "*"); /* Saved choice, rejoined at power-up. */
  }
}

static int8_t progBars(int8_t rssi)
{
  return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
}

static void progWifiRow(unsigned item, row_t *row)
{
  if (item == app.ap_count)
  {
    row->text = "(Scan again)";
    return;
  }
  const net_ap_t *ap = &app.aps[item];
  row->text          = ap->ssid;
  row->bars          = progBars(ap->rssi);
  if (netStorePassword(netSettings(), ap->ssid))
  {
    strcpy(row->right, "*"); /* Password saved after an earlier connection. */
  }
  else if (!ap->secure)
  {
    strcpy(row->right, "o"); /* Open network. */
  }
}

static void progApnRow(unsigned item, row_t *row)
{
  row->text = net_apns[item].label;
  if (netSettings()->mode == NET_MODE_LTE && !strcmp(netSettings()->apn, net_apns[item].apn))
  {
    strcpy(row->right, "*");
  }
}

static void progKbdMenuRow(unsigned item, row_t *row)
{
  row->text = kbdMenuItem(&app.kbd, item);
}

/* "=" same version on the card, "^" another version (update), blank when not installed. */
static void progHubRow(unsigned item, row_t *row)
{
  const hub_project_t        *project = progHubAt(item);
  const prog_catalog_entry_t *entry   = progHubInstalled(project);
  row->text                           = project ? project->name : "";
  if (entry)
  {
    strcpy(row->right, strcmp(entry->version, project->version) ? "^" : "=");
  }
}

/* Footer-sized (11 characters) link summary. "No ESP" is about the module; everything else is about the
 * network, so a responding ESP that has not joined anything reads "ESP ready", not "offline". */
static const char *progNetState(void)
{
  static char       text[12];
  const net_info_t *info = &app.net_info;
  const char       *mode = info->mode == NET_MODE_LTE ? "LTE" : "Wi-Fi";
  if (!netIsPresent())
  {
    return "No ESP";
  }
  if (info->mode == NET_MODE_OFF || info->mode > NET_MODE_LTE || info->state == NET_STATE_IDLE)
  {
    return "ESP ready";
  }
  snprintf(text, sizeof(text), "%s%s", mode,
           info->state == NET_STATE_CONNECTED ? " on" : info->state == NET_STATE_CONNECTING ? "..." : " fail");
  return text;
}

/* A wheel glyph; space has no visible glyph, so it is drawn as an open box. */
static void progDrawWheelChar(int x, int y, char c, bool big)
{
  if (c != ' ')
  {
    lcdPrintf(x, y, white, "%c", c);
    return;
  }
  int w = big ? 9 : 5, h = big ? 4 : 3, bottom = y + (big ? 15 : 8);
  lcdDrawFillRect(x + 1, bottom, w, 1, white);
  lcdDrawFillRect(x + 1, bottom - h, 1, h, white);
  lcdDrawFillRect(x + w, bottom - h, 1, h, white);
}

static void progDrawKeyboard(void)
{
  const kbd_t *k = &app.kbd;
  char         text[24];
  if (k->menu)
  {
    progDrawList("Keyboard", kbdMenuCount(), k->menu_selected, false, progKbdMenuRow);
    progDrawFooter("SEL:OK", "M:Close");
    return;
  }
  snprintf(text, sizeof(text), "%u/%u", k->len, k->max);
  int right = 126 - (int)strlen(text) * 7;
  lcdPrintf(right, 0, white, "%s", text);
  lcdPrintf(2, 0, white, "%.*s", (right - 4) / 7, app.join_ssid);
  lcdDrawFillRect(0, 12, 128, 1, white);
  /* Entry field: the last 16 characters; hidden text shows only the newest character. */
  unsigned first = k->len > 16 ? k->len - 16 : 0;
  for (unsigned i = first; i < k->len; i++)
  {
    char c = k->shown || i + 1 == k->len ? k->text[i] : '*';
    progDrawWheelChar(2 + (int)(i - first) * 7, 15, c, false);
  }
  if ((millis() / SCROLL_MS) % 2)
  {
    lcdDrawFillRect(2 + (int)(k->len - first) * 7, 25, 6, 1, white); /* Blinking cursor. */
  }
  app.marquee = true;
  /* Wheel: the current character large in a box, neighbours small on both sides. */
  lcdDrawFillRoundRect(52, 28, 24, 22, 3, white);
  lcdSetFont(LCD_FONT_11x18);
  char c = kbdChar(k, 0);
  if (c == ' ')
  {
    lcdDrawRect(55, 34, 18, 10, black);
  }
  else
  {
    lcdPrintf(58, 30, black, "%c", c);
  }
  lcdSetFont(LCD_FONT_07x10);
  for (int i = 1; i <= 4; i++)
  {
    progDrawWheelChar(52 - i * 12 + 1, 34, kbdChar(k, -i), false);
    progDrawWheelChar(76 + (i - 1) * 12 + 4, 34, kbdChar(k, i), false);
  }
  progDrawFooter(kbdGroup(k), "Hold SEL:Menu");
}

static void progDraw(void)
{
  if (!app.dirty || !lcdIsInit())
  {
    return;
  }
  app.dirty     = false;
  app.marquee   = false;
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
    progDrawPowerBadge();
  }
  else if (app.screen == MENU)
  {
    progDrawList("Menu", 6, app.menu_selected, true, progMenuRow);
    progDrawFooter("SEL:Open", "M:Auto");
  }
  else if (app.screen == MANUAL)
  {
    progDrawList("Manual", 2, app.manual_selected, true, progManualRow);
    progDrawFooter("SEL:Open", "M:Back");
  }
  else if (app.screen == STORAGE)
  {
    progDrawList("Storage", 2, app.storage_selected, true, progStorageRow);
    progDrawFooter("SEL:Open", "M:Back");
  }
  else if (app.screen == POWER)
  {
    progDrawList("Power", 3, app.power_selected, false, progPowerRow);
    progDrawFooter(app.power_saved ? "SEL:Change" : "Not saved", "M:Back");
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
  else if (app.screen == CHIPS)
  {
    if (!app.mounted && app.pick != PICK_ERASE)
    {
      progDrawHeader(progPickTitle());
      progDrawNoCard(NULL);
      progDrawFooter("", "M:Back");
    }
    else
    {
      progDrawList(progPickTitle(), progChipRows(), app.target, false, progChipRow);
      /* Installed bin is where a card with too many packages is noticed; the rest are left out by ID order. */
      progDrawFooter(app.pick == PICK_AUTO                            ? "SEL:Set"
                     : app.pick == PICK_INSTALLED && app.catalog.more ? "16+ on SD"
                                                                      : "SEL:Open",
                     "M:Back");
    }
  }
  else if (app.screen == PACKAGES)
  {
    const char *title = progTargetLabel((prog_target_t)app.target);
    if (!app.mounted || !progPackageRows())
    {
      progDrawHeader(title);
      if (!app.mounted)
      {
        progDrawNoCard(NULL);
      }
      else
      {
        lcdPrintf(29, 19, white, "No firmware");
        if (app.catalog_error)
        {
          lcdPrintf(2, 35, white, "SD error %u", app.catalog_error);
        }
        else
        {
          lcdPrintf(2, 35, white, "Add files to SD");
        }
      }
      progDrawFooter("", "M:Back");
    }
    else
    {
      progDrawList(title, progPackageRows(), app.selected, false, progPackageRow);
      progDrawFooter(app.pick == PICK_AUTO        ? (app.error[0] ? "Save failed" : "SEL:Save")
                     : app.pick == PICK_INSTALLED ? "SEL:Info"
                                                  : "SEL:Open",
                     "M:Back");
    }
  }
  else if (app.screen == DETAILS)
  {
    const prog_catalog_entry_t *entry = progPackageAt(app.selected);
    char                        text[24];
    /* "*" marks the Auto package, as in the list. */
    snprintf(text, sizeof(text), "%s%s", progTargetLabel(entry->target), progAutoBound(entry) ? " *" : "");
    progDrawHeader(text);
    snprintf(text, sizeof(text), "%u/%u", app.selected + 1, progPackageRows());
    lcdPrintf(126 - (int)strlen(text) * 7, 0, white, "%s", text);
    lcdPrintf(2, 16, white, "%.18s", progScroll(entry->name, 18));
    lcdPrintf(2, 28, white, "%.18s", entry->id);
    if (entry->error)
    {
      lcdPrintf(2, 40, white, "%.18s", progScroll(entry->error, 18));
    }
    else
    {
      snprintf(text, sizeof(text), "%lu B, %u BIN", (unsigned long)entry->size, entry->images);
      lcdPrintf(2, 40, white, "%.18s", text);
    }
    /* Version from release.txt; packages copied before the Hub added it have none. */
    text[0] = 0;
    if (entry->version[0])
    {
      snprintf(text, sizeof(text), "v%.10s", entry->version);
    }
    progDrawFooter(text, "M:Back");
  }
  else if (app.screen == NETWORK)
  {
    progDrawList("Network", 3, app.network_selected, false, progNetworkRow);
    progDrawFooter(progNetState(), "M:Back");
  }
  else if (app.screen == CONNECT)
  {
    progDrawList("Connect", 3, app.connect_selected, false, progConnectRow);
    progDrawFooter("SEL:Open", "M:Back");
  }
  else if (app.screen == WIFI_LIST)
  {
    progDrawList("Wi-Fi", app.ap_count + 1, app.ap_selected, false, progWifiRow);
    progDrawFooter(app.ap_selected == app.ap_count ? "SEL:Scan" : "SEL:Join", "M:Back");
  }
  else if (app.screen == KEYBOARD)
  {
    progDrawKeyboard();
  }
  else if (app.screen == APN_LIST)
  {
    progDrawList("LTE APN", net_apn_count, app.apn_selected, false, progApnRow);
    progDrawFooter("SEL:Join", "M:Back");
  }
  else if (app.screen == NET_STATUS)
  {
    const net_info_t *info = &app.net_info;
    char              text[24];
    progDrawHeader("Status");
    bool joined = netIsPresent() && info->mode != NET_MODE_OFF && info->state != NET_STATE_IDLE;
    lcdPrintf(2, 16, white, "%.18s", !netIsPresent() || joined ? progNetState() : "Not connected");
    if (netIsPresent() && info->rssi)
    {
      snprintf(text, sizeof(text), "%ddBm", info->rssi);
      lcdPrintf(126 - (int)strlen(text) * 7, 16, white, "%s", text);
    }
    lcdPrintf(2, 28, white, "%.18s", netIsPresent() ? progScroll(info->detail, 18) : "Check UART/power");
    if (info->state == NET_STATE_CONNECTED)
    {
      lcdPrintf(2, 40, white, "IP %u.%u.%u.%u", info->ip[0], info->ip[1], info->ip[2], info->ip[3]);
    }
    progDrawFooter(netIsPresent() ? netEspVersion() : "", "M:Back");
  }
  else if (app.screen == HUB_LIST)
  {
    const char *title = progTargetLabel((prog_target_t)app.target);
    if (!app.hub_view_count)
    {
      progDrawHeader(title);
      lcdPrintf(29, 19, white, "No firmware");
      lcdPrintf(2, 35, white, "on the Hub");
      progDrawFooter("", "M:Back");
    }
    else
    {
      progDrawList(title, app.hub_view_count, app.hub_selected, false, progHubRow);
      progDrawFooter("SEL:Open", "M:Back");
    }
  }
  else if (app.screen == HUB_CONFIRM)
  {
    const hub_project_t        *project = progHubAt(app.hub_selected);
    const prog_catalog_entry_t *entry   = progHubInstalled(project);
    const char                 *blocked = progHubBlocked(project);
    char                        text[24];
    progDrawHeader("Install");
    lcdPrintf(2, 16, white, "%.18s", progScroll(project->name, 18));
    snprintf(text, sizeof(text), "v%.10s", project->version);
    lcdPrintf(2, 28, white, "%s", text);
    const char *what = !entry ? "new" : strcmp(entry->version, project->version) ? "update" : "same";
    lcdPrintf(126 - (int)strlen(what) * 7, 28, white, "%s", what);
    lcdPrintf(2, 40, white, "%.18s", blocked ? blocked : progTargetInfo(project->target)->esp ? project->id : "ST loader OK");
    progDrawFooter(blocked ? "" : "SEL:Hold 1s", "M:Back");
  }
  else if (app.screen == ERASE_CONFIRM)
  {
    char title[24];
    snprintf(title, sizeof(title), "Erase / %s", progTargetLabel((prog_target_t)app.target));
    progDrawHeader(title);
    if (!app.mounted && !progIsEspErase())
    {
      progDrawNoCard("Loader on SD");
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
  else if (app.screen == WIPE_CONFIRM)
  {
    progDrawHeader("SD Delete");
    if (!app.mounted)
    {
      progDrawNoCard(NULL);
      progDrawFooter("", "M:Back");
    }
    else
    {
      lcdPrintf(2, 16, white, "Delete ALL files");
      lcdPrintf(2, 28, white, "%u%s firmware", app.catalog.count, app.catalog.more ? "+" : "");
      lcdPrintf(2, 40, white, app.loader_present ? "ST loader too" : "Cannot be undone");
      progDrawFooter("SEL:Hold 1s", "M:Back");
    }
  }
  else if (app.screen == CONFIRM)
  {
    progDrawHeader("Flash / Confirm");
    lcdPrintf(2, 16, white, "%.17s", app.package.manifest.name);
    lcdPrintf(2, 28, white, "%lu B", (unsigned long)app.image_size);
    if (app.package.esp_project)
    {
      lcdPrintf(2, 40, white, "%s / %u BIN", espProjectChipName(app.package.esp.chip), app.package.esp.count);
    }
    else
    {
      lcdPrintf(2, 40, white, "Erase %lu sectors", (unsigned long)((app.image_size + 0x1FFFF) / 0x20000));
    }
    progDrawFooter("SEL:Hold 1s", "M:Back");
  }
  else if (app.screen == RUNNING && (app.job == JOB_SCAN || app.job == JOB_JOIN))
  {
    progDrawRadar();
  }
  else if (app.screen == RUNNING && (app.job == JOB_INSTALL || app.job == JOB_HUB_LIST))
  {
    progDrawInstall();
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

static void progOpen(screen_t screen)
{
  app.screen        = screen;
  app.confirm_armed = false;
  app.hold_start    = 0;
  app.dirty         = true;
}

static void progOpenChips(pick_t pick)
{
  app.pick = pick;
  if (app.target >= progChipRows())
  {
    app.target = 0;
  }
  progOpen(CHIPS);
}

static void progOpenPackages(void)
{
  app.error[0] = 0;
  app.selected = 0;
  progViewBuild();
  for (unsigned row = 1; app.pick == PICK_AUTO && row < progPackageRows(); row++)
  {
    if (progAutoBound(progPackageAt(row)))
    {
      app.selected = row; /* Start on the current binding. */
    }
  }
  progOpen(PACKAGES);
}

/* Keyboard buttons: UP/DN repeat while held, SEL adds on release or opens the menu when held, M deletes. */
static void progKeyboardKeys(uint32_t buttons, uint32_t pressed, uint32_t released)
{
  uint32_t     now    = millis();
  kbd_result_t result = KBD_EDIT;
  for (unsigned b = 0; b < 2; b++)
  {
    uint32_t  mask = 1U << (b ? BTN_DN : BTN_UP);
    kbd_key_t key  = b ? KBD_KEY_DN : KBD_KEY_UP;
    if (pressed & mask)
    {
      app.repeat_at = now + REPEAT_MS;
      kbdKey(&app.kbd, key);
      app.dirty = true;
    }
    else if ((buttons & mask) && (int32_t)(now - app.repeat_at) >= 0)
    {
      app.repeat_at = now + REPEAT_STEP_MS;
      kbdKey(&app.kbd, key);
      app.dirty = true;
    }
  }
  if (pressed & (1U << BTN_SEL))
  {
    app.sel_down = now;
    app.sel_used = false;
  }
  if ((buttons & (1U << BTN_SEL)) && !app.sel_used && (uint32_t)(now - app.sel_down) >= MENU_HOLD_MS)
  {
    app.sel_used = true;
    result       = kbdKey(&app.kbd, KBD_KEY_MENU);
    app.dirty    = true;
  }
  if ((released & (1U << BTN_SEL)) && !app.sel_used)
  {
    result    = kbdKey(&app.kbd, KBD_KEY_ADD);
    app.dirty = true;
  }
  if (released & (1U << BTN_M))
  {
    result    = kbdKey(&app.kbd, KBD_KEY_DEL);
    app.dirty = true;
  }
  if (result == KBD_CANCEL)
  {
    progOpen(WIFI_LIST);
  }
  else if (result == KBD_DONE)
  {
    snprintf(app.join_secret, sizeof(app.join_secret), "%s", app.kbd.text);
    app.join_mode = NET_MODE_WIFI;
    app.job       = JOB_JOIN;
    progRunJob();
  }
}

/* Open networks join at once; a saved password is offered for editing, so a changed one can be fixed. */
static void progSelectWifi(void)
{
  if (app.ap_selected == app.ap_count)
  {
    app.job = JOB_SCAN;
    progRunJob();
    return;
  }
  const net_ap_t *ap = &app.aps[app.ap_selected];
  snprintf(app.join_ssid, sizeof(app.join_ssid), "%s", ap->ssid);
  if (!ap->secure)
  {
    app.join_secret[0] = 0;
    app.join_mode      = NET_MODE_WIFI;
    app.job            = JOB_JOIN;
    progRunJob();
    return;
  }
  const char *saved = netStorePassword(netSettings(), ap->ssid);
  kbdBegin(&app.kbd, NET_PASSWORD_SIZE - 1, saved, false);
  app.sel_used = true; /* The SEL press that opened the keyboard must not type a character. */
  progOpen(KEYBOARD);
}

static void progOpenHubList(void)
{
  app.hub_view_count = progHubCount((prog_target_t)app.target, app.hub_view);
  app.hub_selected   = 0;
  progOpen(HUB_LIST);
}

/* Where SEL or M on a manual RESULT returns to. */
static screen_t progResultBack(void)
{
  switch (app.job)
  {
    case JOB_FLASH:
      return PACKAGES;
    case JOB_ERASE:
      return CHIPS;
    case JOB_WIPE:
      return STORAGE;
    case JOB_SCAN:
      return CONNECT;
    case JOB_JOIN:
      return !app.error[0] ? NETWORK : app.join_mode == NET_MODE_WIFI ? WIFI_LIST : APN_LIST;
    case JOB_INSTALL:
      return HUB_LIST;
    default:
      return NETWORK;
  }
}

/* UP/DN wrap around a list; true when the cursor moved. */
static bool progStep(unsigned *cursor, unsigned count, uint32_t pressed)
{
  if (!count || !(pressed & ((1U << BTN_UP) | (1U << BTN_DN))))
  {
    return false;
  }
  *cursor   = (pressed & (1U << BTN_UP)) ? (*cursor + count - 1) % count : (*cursor + 1) % count;
  app.dirty = true;
  return true;
}

/* Destructive jobs start only after SEL is released and then held for a second. */
static void progHold(uint32_t buttons, bool ready)
{
  if (!ready)
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

static void progSelectPackage(void)
{
  const prog_catalog_entry_t *entry = progPackageAt(app.selected);
  if (app.pick == PICK_AUTO)
  {
    if (progAutoSave(entry ? entry->id : ""))
    {
      progOpen(CHIPS);
      buzzerBeep(60);
    }
    else
    {
      snprintf(app.error, sizeof(app.error), "Save failed");
      app.dirty = true;
    }
    return;
  }
  if (app.pick == PICK_INSTALLED)
  {
    progOpen(DETAILS);
    return;
  }
  app.job      = JOB_FLASH;
  app.stage    = "OPEN PROJECT";
  app.error[0] = 0;
  memset(&app.result, 0, sizeof(app.result));
  strcpy(app.job_id, entry->id);
  bool ok = progLoadManifest(app.job_id);
  if (ok && !app.package.esp_project)
  {
    char    path[128];
    FILINFO info;
    snprintf(path, sizeof(path), "/prog/fw/%s/%s", app.job_id, app.package.manifest.image);
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
  progOpen(ok ? CONFIRM : RESULT);
}

void progUpdate(void)
{
  bool detected = sdIsDetected(), mounted = detected && fatfsIsMount();
  if (mounted != app.mounted || detected != app.detected)
  {
    bool card    = mounted && !app.mounted;
    app.mounted  = mounted;
    app.detected = detected;
    app.dirty    = true;
    progAutoLoad();
    if (card)
    {
      progPowerReload();
    }
    netSettingsReload(mounted); /* Saved networks belong to the card. */
    /* Keep the result visible even when the card was removed during a job. */
    if (app.screen != RESULT)
    {
      progRefresh();
    }
    if (app.screen == CONFIRM || app.screen == DETAILS)
    {
      app.screen = PACKAGES;
    }
    if (app.screen == HUB_CONFIRM)
    {
      app.screen = HUB_LIST;
    }
    app.confirm_armed = false;
    app.hold_start    = 0;
  }
  uint32_t buttons = buttonGetData(), pressed = buttons & ~app.last_buttons, released = app.last_buttons & ~buttons;
  bool     back = (pressed & (1U << BTN_M)) != 0, open = (pressed & (1U << BTN_SEL)) != 0;
  app.last_buttons = buttons;
  if ((app.screen == NETWORK || app.screen == NET_STATUS) && (uint32_t)(millis() - app.net_poll) >= NET_POLL_MS)
  {
    app.net_poll = millis();
    app.net_info = *netInfoCached(); /* Never a link call here: a missing ESP must not stall the screen. */
    app.dirty    = true;
  }
  switch (app.screen)
  {
    case HOME:
      if (back)
      {
        progOpen(MENU);
      }
      else if (open && app.mounted)
      {
        if (progAutoAssigned())
        {
          progRunAuto();
        }
        else
        {
          progOpenChips(PICK_AUTO);
        }
      }
      break;
    case MENU:
      if (back)
      {
        progOpen(HOME);
      }
      else if (!progStep(&app.menu_selected, 6, pressed) && open)
      {
        static const screen_t next[] = {CHIPS, MANUAL, STORAGE, NETWORK, POWER, INFO};
        app.auto_job                 = false;
        if (app.menu_selected == 0)
        {
          progOpenChips(PICK_AUTO);
        }
        else
        {
          app.net_poll = millis() - NET_POLL_MS; /* Network pages show fresh state at once. */
          progOpen(next[app.menu_selected]);
        }
      }
      break;
    case MANUAL:
      if (back)
      {
        progOpen(MENU);
      }
      else if (!progStep(&app.manual_selected, 2, pressed) && open)
      {
        progOpenChips(app.manual_selected ? PICK_ERASE : PICK_FLASH);
      }
      break;
    case STORAGE:
      if (back)
      {
        progOpen(MENU);
      }
      else if (!progStep(&app.storage_selected, 2, pressed) && open)
      {
        if (app.storage_selected)
        {
          app.job = JOB_WIPE;
          progOpen(WIPE_CONFIRM);
        }
        else
        {
          progOpenChips(PICK_INSTALLED);
        }
      }
      break;
    case POWER:
      if (back)
      {
        progOpen(MENU);
      }
      else if (!progStep(&app.power_selected, 3, pressed) && open)
      {
        if (app.power_selected == 0)
        {
          app.power.out_3v3 = !app.power.out_3v3;
        }
        else if (app.power_selected == 1)
        {
          app.power.out_5v = !app.power.out_5v;
        }
        else
        {
          app.power.mode = app.power.mode == PROG_POWER_ALWAYS ? PROG_POWER_JOB : PROG_POWER_ALWAYS;
        }
        progPowerChange();
        buzzerBeep(app.power_saved ? 60 : 400);
      }
      break;
    case INFO:
      if (back)
      {
        progOpen(MENU);
      }
      break;
    case CHIPS:
      if (back)
      {
        progOpen(app.pick == PICK_AUTO        ? MENU
                 : app.pick == PICK_INSTALLED ? STORAGE
                 : app.pick == PICK_HUB       ? NETWORK
                                              : MANUAL);
      }
      else if ((app.mounted || app.pick == PICK_ERASE || app.pick == PICK_HUB) &&
               !progStep(&app.target, progChipRows(), pressed) && open)
      {
        if (app.pick == PICK_ERASE)
        {
          app.job = JOB_ERASE;
          progOpen(ERASE_CONFIRM);
        }
        else if (app.pick == PICK_HUB)
        {
          progOpenHubList();
        }
        else
        {
          progOpenPackages();
        }
      }
      break;
    case PACKAGES:
      if (back)
      {
        progOpen(CHIPS);
      }
      else if (app.mounted && !progStep(&app.selected, progPackageRows(), pressed) && open && progPackageRows())
      {
        progSelectPackage();
      }
      break;
    case DETAILS:
      if (back)
      {
        progOpen(PACKAGES);
      }
      else
      {
        progStep(&app.selected, progPackageRows(), pressed);
      }
      break;
    case NETWORK:
      if (back)
      {
        progOpen(MENU);
      }
      else if (!progStep(&app.network_selected, 3, pressed) && open)
      {
        if (app.network_selected == 0)
        {
          progOpen(CONNECT);
        }
        else if (app.network_selected == 1)
        {
          app.job = JOB_HUB_LIST;
          progRunJob();
        }
        else
        {
          progOpen(NET_STATUS);
        }
      }
      break;
    case CONNECT:
      if (back)
      {
        progOpen(NETWORK);
      }
      else if (!progStep(&app.connect_selected, 3, pressed) && open)
      {
        if (app.connect_selected == 0)
        {
          app.job = JOB_SCAN;
          progRunJob();
        }
        else if (app.connect_selected == 1)
        {
          app.apn_selected = 0;
          for (unsigned i = 0; i < net_apn_count; i++)
          {
            if (!strcmp(netSettings()->apn, net_apns[i].apn))
            {
              app.apn_selected = i;
            }
          }
          progOpen(APN_LIST);
        }
        else
        {
          uint8_t status        = netOff();
          netSettings()->mode   = NET_MODE_OFF;
          app.job               = JOB_NET_OFF;
          app.auto_job          = false;
          bool ok               = status == NET_ST_OK || progFail(netStatusText(status));
          netSettingsSave();
          progFinish(ok);
        }
      }
      break;
    case WIFI_LIST:
      if (back)
      {
        progOpen(CONNECT);
      }
      else if (!progStep(&app.ap_selected, app.ap_count + 1, pressed) && open)
      {
        progSelectWifi();
      }
      break;
    case KEYBOARD:
      progKeyboardKeys(buttons, pressed, released);
      break;
    case APN_LIST:
      if (back)
      {
        progOpen(CONNECT);
      }
      else if (!progStep(&app.apn_selected, net_apn_count, pressed) && open)
      {
        snprintf(app.join_secret, sizeof(app.join_secret), "%s", net_apns[app.apn_selected].apn);
        app.join_mode = NET_MODE_LTE;
        app.job       = JOB_JOIN;
        progRunJob();
      }
      break;
    case NET_STATUS:
      if (back)
      {
        progOpen(NETWORK);
      }
      break;
    case HUB_LIST:
      if (back)
      {
        progOpen(CHIPS);
      }
      else if (!progStep(&app.hub_selected, app.hub_view_count, pressed) && open && app.hub_view_count)
      {
        app.job = JOB_INSTALL;
        progOpen(HUB_CONFIRM);
      }
      break;
    case HUB_CONFIRM:
      if (back)
      {
        progOpen(HUB_LIST);
      }
      else
      {
        progHold(buttons, !progHubBlocked(progHubAt(app.hub_selected)));
      }
      break;
    case CONFIRM:
    case ERASE_CONFIRM:
    case WIPE_CONFIRM:
      if (back)
      {
        progOpen(app.screen == CONFIRM ? PACKAGES : app.screen == ERASE_CONFIRM ? CHIPS : STORAGE);
      }
      else
      {
        progHold(buttons, app.mounted || progIsEspErase());
      }
      break;
    case RESULT:
      if (!back && !open)
      {
        break;
      }
      if (app.auto_job)
      {
        if (back || !app.error[0]) /* PASS returns home; the next unit starts from there. */
        {
          app.auto_job = false;
          progOpen(HOME);
        }
        else
        {
          progRunAuto();
        }
      }
      else
      {
        progRefresh();
        progOpen(progResultBack());
      }
      break;
    case RUNNING:
      break;
  }
  uint32_t idle = millis() - app.draw_time;
  if ((idle >= FRAME_MS &&
       (progAnim().active || (app.sel_screen == app.screen && (uint32_t)(millis() - app.sel_start) < SLIDE_MS + FRAME_MS))) ||
      (app.marquee && idle >= SCROLL_MS))
  {
    app.dirty = true;
  }
  progDraw();
}

/* CLI changes the catalog or the loaded package underneath the OLED, so screens that show them start over. */
static void progCliReset(void)
{
  if (app.screen == CONFIRM || app.screen == ERASE_CONFIRM || app.screen == WIPE_CONFIRM || app.screen == RESULT ||
      app.screen == DETAILS)
  {
    app.auto_job = false;
    progOpen(HOME);
  }
  app.confirm_armed = false;
  app.dirty         = true;
}

static void cliProg(cli_args_t *args)
{
  if (args->argc == 1 && args->isStr(0, "list"))
  {
    progRefresh();
    progCliReset();
    if (app.catalog_error)
    {
      cliPrintf("SD error %u while reading /prog/fw\n", app.catalog_error);
    }
    for (unsigned i = 0; i < app.catalog.count; i++)
    {
      const prog_catalog_entry_t *entry = &app.catalog.entries[i];
      const prog_target_info_t   *chip  = progTargetInfo(entry->target);
      cliPrintf("%-10s %-24s %-12s %s%s%s\n", chip ? chip->chip : "unknown", entry->id,
                entry->version[0] ? entry->version : "-", entry->name, entry->error ? "  ! " : "",
                entry->error ? entry->error : "");
    }
    cliPrintf("%u projects%s\n", app.catalog.count,
              app.catalog.more ? " (limit 16; smallest IDs kept, more omitted)" : "");
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
    progCliReset();
    return;
  }
  cliPrintf("prog list\nprog check <project>\nUse OLED: UP/DN, SEL, then hold SEL 1s to write.\n");
}

static void cliEsp(cli_args_t *args)
{
  if (args->argc == 1 && args->isStr(0, "info"))
  {
    cliPrintf("ESP pins: CLK/PB4=TX, SWO/PC1=RX, nRESET/PB3=EN, DIO/PB1=BOOT, GND\n"
              "Connect at 115200 8N1; Flash/Erase link test up to 921600. Target supply: see 'power'.\n"
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

/* Same settings as the Power menu, saved to the card the same way. */
static void cliPower(cli_args_t *args)
{
  bool output = args->argc == 2 && (args->isStr(0, "3v3") || args->isStr(0, "5v"));
  if (output && (args->isStr(1, "on") || args->isStr(1, "off")))
  {
    bool *out = args->isStr(0, "3v3") ? &app.power.out_3v3 : &app.power.out_5v;
    *out      = args->isStr(1, "on");
    progPowerChange();
  }
  else if (args->argc == 2 && args->isStr(0, "mode") && (args->isStr(1, "job") || args->isStr(1, "always")))
  {
    app.power.mode = args->isStr(1, "always") ? PROG_POWER_ALWAYS : PROG_POWER_JOB;
    progPowerChange();
  }
  else if (args->argc != 0)
  {
    cliPrintf("power\npower 3v3|5v on|off\npower mode job|always\n");
    return;
  }
  cliPrintf("SETTING  : 3V3=%s 5V=%s, supply=%s, %s\n", app.power.out_3v3 ? "ON" : "OFF",
            app.power.out_5v ? "ON" : "OFF", app.power.mode == PROG_POWER_ALWAYS ? "always" : "during jobs",
            app.power_saved ? "saved on SD" : "NOT saved (no SD)");
  cliPrintf("OUTPUT   : 3V_OUT(PB5)=%s 5V_OUT(PA9)=%s\n", gpioPinRead(TARGET_3V3) ? "ON" : "OFF",
            gpioPinRead(TARGET_5V) ? "ON" : "OFF");
}

/* Keeps network job animations moving while a request waits on the ESP; drawing only, no input. */
static void progNetIdle(void)
{
  if (app.screen == RUNNING && (uint32_t)(millis() - app.draw_time) >= RUN_FRAME_MS)
  {
    app.dirty = true;
    progDraw();
  }
}

static bool progInit(void)
{
  netSetIdleHook(progNetIdle);
  app.dirty         = true;
  app.stage         = "IDLE";
  app.last_buttons  = buttonGetData();
  app.screen        = HOME;
  app.menu_selected = 0;
  app.detected      = sdIsDetected();
  app.mounted       = app.detected && fatfsIsMount();
  progRefresh();
  progAutoLoad();
  app.power_saved = true; /* Nothing to save yet: all off, as the gate pull-downs hold it. */
  if (app.mounted)
  {
    progPowerReload();
  }
  bool ok = cliAdd("prog", cliProg);
  ok      = cliAdd("power", cliPower) && ok;
  return cliAdd("esp", cliEsp) && ok;
}

MODULE_DEF(prog)
{
  .name     = "prog",
  .priority = MODULE_PRI_HIGH,
  .init     = progInit,
};
