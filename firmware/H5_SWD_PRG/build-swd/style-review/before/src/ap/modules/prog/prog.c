#include "module.h"
#include "fatfs.h"
#include "prog.h"
#include "prog_inspect.h"
#include "swd/swd_program.h"
#include <string.h>

#define PROJECTS 16
#define ID_SIZE 25
#define LOADER_PATH "/prog/loaders/st/0x483.stldr"
#define LOADER_CRC 0x32674EA9U
typedef enum { HOME, FILES, INFO, CONFIRM, ERASE_CONFIRM, RUNNING, RESULT } screen_t;
typedef struct { FIL file; FRESULT error; } sd_file_t;
static struct {
  screen_t screen;
  char ids[PROJECTS][ID_SIZE];
  unsigned count, selected, home_selected;
  bool erase_job, mounted, detected, dirty, confirm_armed, more, cancel, image_open;
  uint32_t last_buttons, hold_start, draw_time, job_start, done, total;
  const char *stage;
  char error[96];
  prog_manifest_t manifest;
  prog_loader_t loader;
  prog_image_t image;
  swd_program_result_t result;
  sd_file_t image_file;
  uint32_t image_size;
  uint32_t code[1024];
} app;

static void draw(void);
static bool validId(const char *s)
{
  size_t n=strlen(s);
  if (!n || n>=ID_SIZE) return false;
  for (;*s;s++) if (!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||
                          (*s>='0'&&*s<='9')||*s=='_'||*s=='-')) return false;
  return true;
}
static bool fail(const char *message)
{
  snprintf(app.error,sizeof(app.error),"%s",message);
  cliPrintf("FAILED   : %s\n",message);
  return false;
}
static bool ioFail(const char *path, FRESULT error)
{
  snprintf(app.error,sizeof(app.error),"SD error %u",error);
  cliPrintf("FAILED   : %s (FatFs=%u)\n",path,error);
  return false;
}
static bool readFile(void *ctx,uint32_t offset,void *buf,uint32_t size)
{
  sd_file_t *s=ctx;
  UINT got=0;
  ledActivityPulse();
  s->error=f_lseek(&s->file,offset);
  if(s->error==FR_OK)s->error=f_read(&s->file,buf,size,&got);
  ledActivityPulse();
  if(s->error==FR_OK && got!=size)s->error=FR_INT_ERR;
  return s->error==FR_OK;
}
static bool openFile(sd_file_t *s,const char *path,prog_file_t *file)
{
  s->error=f_open(&s->file,path,FA_READ);
  if(s->error!=FR_OK)return ioFail(path,s->error);
  *file=(prog_file_t){s,(uint32_t)f_size(&s->file),readFile};
  return true;
}
static bool closeFile(sd_file_t *s)
{
  FRESULT e=f_close(&s->file);
  return e==FR_OK ? true : ioFail("close",e);
}
static bool closeImage(void)
{
  if(!app.image_open)return true;
  app.image_open=false;
  return closeFile(&app.image_file);
}
static bool manifest(const char *id)
{
  sd_file_t s;
  prog_file_t f;
  char path[96],text[1025];
  if(!validId(id))return fail("Invalid project ID");
  snprintf(path,sizeof(path),"/prog/fw/%s/fw.txt",id);
  if(!openFile(&s,path,&f))return false;
  const char *error=NULL;
  if(!f.size || f.size>1024)error="fw.txt size";
  else if(!readFile(&s,0,text,f.size))error="fw.txt read";
  else if(memchr(text,0,f.size))error="fw.txt NUL";
  else {
    text[f.size]=0;
    error=progParseManifest(text,&app.manifest);
  }
  bool closed=closeFile(&s);
  if(error)return fail(error);
  return closed;
}
static void scan(void)
{
  app.count=app.selected=0; app.more=false; app.error[0]=0;
  app.dirty=true;
  if(!app.mounted)return;
  DIR dir;
  FILINFO info;
  FRESULT e=f_opendir(&dir,"/prog/fw");
  if(e!=FR_OK) { ioFail("/prog/fw",e); return; }
  while((e=f_readdir(&dir,&info))==FR_OK && info.fname[0]) {
    if(!(info.fattrib&AM_DIR)||!validId(info.fname))continue;
    char path[96]; FILINFO file;
    snprintf(path,sizeof(path),"/prog/fw/%.24s/fw.txt",info.fname);
    if(f_stat(path,&file)!=FR_OK || (file.fattrib&AM_DIR))continue;
    if(app.count==PROJECTS) { app.more=true; continue; }
    strcpy(app.ids[app.count++],info.fname);
  }
  FRESULT ce=f_closedir(&dir);
  if(e!=FR_OK || ce!=FR_OK) { app.count=0; ioFail("scan",e!=FR_OK?e:ce); }
  for(unsigned i=0;i<app.count;i++)for(unsigned j=i+1;j<app.count;j++)
    if(strcmp(app.ids[i],app.ids[j])>0) {
      char tmp[ID_SIZE]; strcpy(tmp,app.ids[i]); strcpy(app.ids[i],app.ids[j]); strcpy(app.ids[j],tmp);
    }
}
static bool progress(void *context,const char *step,uint32_t done,uint32_t total)
{
  (void)context;
  app.stage=step; app.done=done; app.total=total;
  if(buttonGetPressed(BTN_M) || !sdIsDetected())app.cancel=true;
  if((uint32_t)(millis()-app.draw_time)>=200) {
    app.dirty=true; draw();
  }
  delay(1);
  return !app.cancel;
}
static bool readImage(void *ctx,uint32_t off,void *buf,uint32_t n)
{
  (void)ctx;
  if(!sdIsDetected())return fail("SD removed");
  if(!readFile(&app.image_file,off,buf,n))return ioFail("app.bin read",app.image_file.error);
  return true;
}
static bool loadLoader(const char *path,uint32_t expected_crc)
{
  if(!sdIsDetected() || !fatfsIsMount())return fail("SD not mounted");
  sd_file_t s; prog_file_t f;
  uint32_t crc=0;
  if(!openFile(&s,path,&f))return false;
  const char *error=progInspectLoader(&f,&app.loader);
  if(!error && !progFileCrc32(&f,&crc))error="Loader read failed";
  if(!error && (crc!=expected_crc || crc!=LOADER_CRC))error="Unsupported loader CRC";
  if(!error && (app.loader.code_start!=0x24000004 || app.loader.code_end!=0x240007E8 ||
               app.loader.code_filesz!=2020))error="Unsupported loader layout";
  if(!error && !readFile(&s,app.loader.code_offset,app.code,app.loader.code_filesz))error="Loader code read";
  bool closed=closeFile(&s);
  if(error)return fail(error);
  if(!closed)return false;
  cliPrintf("LOADER   : %s, CRC32=%08lX OK\n",path,(unsigned long)crc);
  return true;
}
static bool preflight(const char *id)
{
  app.error[0]=0;
  if(!fatfsIsMount())return fail("SD not mounted");
  if(!manifest(id))return false;
  cliPrintf("PROJECT  : %s\n",app.manifest.name);
  if(!loadLoader(app.manifest.algo,app.manifest.loader_crc32))return false;
  prog_file_t f;
  uint32_t crc=0;
  const char *error=NULL;
  char path[128];
  snprintf(path,sizeof(path),"/prog/fw/%s/%s",id,app.manifest.image);
  if(!openFile(&app.image_file,path,&f))return false;
  app.image_open=true; app.image_size=f.size;
  error=progInspectImage(&f,app.manifest.address,&app.image);
  if(!error && !progFileCrc32(&f,&crc))error="Image read failed";
  if(!error && crc!=app.manifest.crc32)error="Image CRC mismatch";
  if(error) { closeImage(); return fail(error); }
  cliPrintf("IMAGE    : %s, %lu bytes, CRC32=%08lX OK\n",path,(unsigned long)f.size,(unsigned long)crc);
  cliPrintf("ADDRESS  : %08lX SP=%08lX Reset=%08lX\n",(unsigned long)app.manifest.address,
            (unsigned long)app.image.sp,(unsigned long)app.image.reset);
  return true;
}
static void runJob(void)
{
  app.screen=RUNNING; app.stage="CHECK SD FILES";
  app.cancel=false; app.done=0; app.total=1; app.job_start=millis();
  memset(&app.result,0,sizeof(app.result));
  app.dirty=true; draw();
  app.error[0]=0;
  if(app.erase_job)cliPrintf("ERASE    : H723 internal flash, 08000000..080FFFFF, all 8 sectors\n");
  bool ok=app.erase_job?loadLoader(LOADER_PATH,LOADER_CRC):preflight(app.ids[app.selected]);
  if(ok && !progress(NULL,"CHECK SD FILES",1,1))ok=fail("Stopped before reset");
  if(ok) {
    swd_program_input_t input={app.code,app.loader.code_filesz,app.erase_job?0:app.image_size,
                              app.erase_job?0:app.manifest.crc32,NULL,app.erase_job?NULL:readImage,progress};
    swd_err_t e=app.erase_job?swdEraseH723(&input,&app.result):swdProgramH723(&input,&app.result);
    ok=(e==SWD_OK);
    if(!ok && !app.error[0])fail(app.result.cancelled?"Stopped":swdErrStr(e));
    cliPrintf("%s : %s at %s, address=%08lX, SR=%08lX\n",app.erase_job?"ERASE   ":"PROGRAM ",swdErrStr(e),app.result.stage,
              (unsigned long)app.result.address,(unsigned long)app.result.flash_sr);
    if(!ok) {
      cliPrintf("STATUS   : SR=%s; flash status error=%u\n",
                app.result.flash_sr_valid?"current":"previous/unavailable",
                (unsigned)app.result.flash_status_error);
      if(app.result.failed_io)
        cliPrintf("FAIL IO  : %s @%08lX ACK=%lu CTRL(last)=%08lX\n",app.result.failed_io,
                  (unsigned long)app.result.failed_io_address,(unsigned long)app.result.failed_ack,
                  (unsigned long)app.result.failed_ctrl);
    }
    cliPrintf("CORE     : PC=%08lX DFSR=%08lX R0=%lu; %s\n",(unsigned long)app.result.pc,
              (unsigned long)app.result.dfsr,(unsigned long)app.result.result,
              app.result.started?"new firmware started":app.result.erase_complete?"held halted (flash verified blank)":app.result.halted_on_error?"held halted":
              app.result.reset_attempted?"state not confirmed":"no programming reset");
    cliPrintf("BYTES    : written=%lu verified=%lu; elapsed=%lu s\n",(unsigned long)app.result.written,
              (unsigned long)app.result.verified,(unsigned long)(app.result.elapsed_ms/1000));
    cliPrintf("SPEED    : clock ceiling=%lu kHz, link test=%lu ms\n",
              (unsigned long)app.result.clock_khz,(unsigned long)app.result.link_test_ms);
    cliPrintf("TIMING   : erase=%lu ms, write=%lu ms, verify=%lu ms\n",
              (unsigned long)app.result.erase_ms,(unsigned long)app.result.write_ms,(unsigned long)app.result.verify_ms);
  }
  if(!closeImage())ok=false;
  ledActivityStop();
  if(ok)app.error[0]=0;
  app.screen=RESULT; app.dirty=true;
  app.last_buttons=buttonGetData();
  draw();
}
/* Pixel-aligned artwork for the 128x64 monochrome OLED. */
static const uint16_t icon_flash[12]={0x060,0x0C0,0x180,0x300,0x7F0,0xFE0,0x0C0,0x180,0x300,0x600,0x400,0};
static const uint16_t icon_erase[12]={0x0F0,0x3FC,0x000,0x204,0x294,0x294,0x294,0x294,0x294,0x204,0x204,0x3FC};
static const uint16_t icon_info[12]={0x1F8,0x204,0x402,0x861,0x801,0x861,0x861,0x861,0x861,0x402,0x204,0x1F8};
static const uint16_t icon_card[12]={0x1FE,0x202,0x422,0x8AA,0x8AA,0x802,0x802,0x802,0x802,0x802,0x802,0xFFE};
static const uint16_t icon_check[12]={0,0,0x003,0x007,0x00E,0x81C,0xC38,0x670,0x3E0,0x1C0,0x080,0};
static void icon(int x,int y,const uint16_t *rows,uint16_t color,unsigned scale)
{
  for(unsigned r=0;r<12;r++)for(unsigned c=0;c<12;c++)
    if(rows[r] & (1U<<(11-c)))lcdDrawFillRect(x+c*scale,y+r*scale,scale,scale,color);
}
static void header(const char *title)
{
  lcdPrintf(2,0,white,"%s",title);
  lcdDrawFillRect(0,12,128,1,white);
}
static void footer(const char *left,const char *right)
{
  lcdDrawFillRect(0,52,128,1,white);
  lcdPrintf(2,54,white,"%s",left);
  lcdPrintf(126-(int)strlen(right)*7,54,white,"%s",right);
}
static void selection(int y,int height)
{
  lcdDrawFillRect(2,y,120,height,white);
  lcdDrawFillRect(1,y+1,122,height-2,white);
}
static void draw(void)
{
  if(!app.dirty || !lcdIsInit())return;
  app.dirty=false; app.draw_time=millis();
  lcdSetFont(LCD_FONT_07x10); lcdClearBuffer(black);
  if(app.screen==HOME) {
    header("H5-SWD-Prog");
    if(app.mounted)lcdDrawFillRect(104,0,24,11,white);
    else lcdDrawRect(104,0,24,11,white);
    lcdPrintf(109,0,app.mounted?black:white,"SD");
    const char *items[]={"Flash","Erase","Info"};
    for(unsigned row=0;row<3;row++) {
      int y=15+row*12;
      bool sel=row==app.home_selected;
      uint16_t color=sel?black:white;
      if(sel)selection(y,12);
      icon(8,y,row==0?icon_flash:row==1?icon_erase:icon_info,color,1);
      lcdPrintf(29,y+1,color,"%s",items[row]);
      if(sel)lcdPrintf(111,y+1,color,">");
    }
    footer("UP/DN","SEL:Open");
  } else if(app.screen==INFO) {
    header("H5-SWD-Prog");
    lcdPrintf(39,16,white,"made by");
    lcdSetFont(LCD_FONT_11x18);
    lcdPrintf(25,29,white,"Higenis");
    lcdSetFont(LCD_FONT_07x10);
    footer("Info","M:Back");
  } else if(app.screen==FILES) {
    header("Flash");
    if(!app.mounted) {
      icon(5,21,icon_card,white,2);
      lcdPrintf(39,19,white,app.detected?"SD not ready":"No SD card");
      lcdPrintf(39,34,white,app.detected?"Check FAT32":"Insert card");
    } else if(!app.count) {
      lcdPrintf(29,19,white,"No firmware");
      lcdPrintf(2,35,white,"%.17s",app.error[0]?app.error:"Add files to SD");
    } else {
      char count[24];snprintf(count,sizeof(count),"%u/%u",app.selected+1,app.count);
      lcdPrintf(126-(int)strlen(count)*7,0,white,"%s",count);
      unsigned first=(app.selected/3)*3;
      for(unsigned row=0;row<3 && first+row<app.count;row++) {
        bool sel=first+row==app.selected;
        int y=15+row*12;
        if(sel)selection(y,12);
        lcdPrintf(5,y+1,sel?black:white,"%c%.15s",sel?'>':' ',app.ids[first+row]);
      }
      if(app.count>3) {
        lcdDrawFillRect(126,15,1,36,white);
        unsigned height=36*3/app.count;
        unsigned y=15+(36-height)*app.selected/(app.count-1);
        lcdDrawFillRect(125,y,3,height,white);
      }
    }
    footer(app.mounted && app.count?"SEL:Open":"","M:Back");
  } else if(app.screen==ERASE_CONFIRM) {
    header("Erase / H723");
    if(!app.mounted) {
      icon(5,21,icon_card,white,2);
      lcdPrintf(39,19,white,app.detected?"SD not ready":"No SD card");
      lcdPrintf(39,34,white,"Loader on SD");
      footer("","M:Back");
    } else {
      lcdPrintf(2,16,white,"ALL FLASH: 1 MiB");
      lcdPrintf(2,28,white,"Erase 8 sectors");
      lcdPrintf(2,40,white,"Deletes firmware");
      footer("SEL:Hold 1s","M:Back");
    }
  } else if(app.screen==CONFIRM) {
    header("Flash / Confirm");
    lcdPrintf(2,16,white,"%.17s",app.manifest.name);
    lcdPrintf(2,28,white,"%lu B",(unsigned long)app.image_size);
    lcdPrintf(2,40,white,"Erase %lu sectors",(unsigned long)((app.image_size+0x1FFFF)/0x20000));
    footer("SEL:Hold 1s","M:Back");
  } else if(app.screen==RUNNING) {
    header(app.erase_job?"Erasing":"Flashing");
    lcdPrintf(2,15,white,"%.17s",app.stage);
    unsigned pct=app.total?(unsigned)(((uint64_t)app.done*100)/app.total):0;
    if(pct>100)pct=100;
    lcdSetFont(LCD_FONT_11x18);
    lcdPrintf(2,27,white,"%u%%",pct);
    lcdSetFont(LCD_FONT_07x10);
    unsigned long seconds=(millis()-app.job_start)/1000;
    if(seconds>99999)seconds=99999;
    char elapsed[12];snprintf(elapsed,sizeof(elapsed),"%lus",seconds);
    lcdPrintf(126-(int)strlen(elapsed)*7,33,white,"%s",elapsed);
    lcdDrawRect(2,47,124,4,white);
    if(pct)lcdDrawFillRect(3,48,(int)(pct*122/100),2,white);
    footer(app.cancel?"Stopping...":"","M:Stop");
  } else {
    header(app.erase_job?(app.error[0]?"Erase / Failed":"Erase / Complete"):
                         (app.error[0]?"Flash / Failed":"Flash / Complete"));
    if(app.error[0]) {
      lcdPrintf(2,15,white,"%.17s",app.result.stage?app.result.stage:app.stage);
      lcdPrintf(2,27,white,"%.17s",app.error);
      lcdPrintf(2,39,white,"@%08lX",(unsigned long)app.result.address);
      footer(app.result.halted_on_error?"HALTED":"See CLI",app.erase_job?"M:Home":"M:Files");
    } else {
      icon(6,21,icon_check,white,2);
      lcdPrintf(38,16,white,app.erase_job?"Blank OK":"Verified");
      lcdPrintf(38,28,white,"%lu B",(unsigned long)app.result.verified);
      lcdPrintf(38,40,white,"%lu sec",(unsigned long)(app.result.elapsed_ms/1000));
      footer(app.erase_job?"HALTED":"",app.erase_job?"M:Home":"SEL/M:Files");
    }
  }
  lcdUpdateDraw();
}
void progUpdate(void)
{
  bool detected=sdIsDetected(), mounted=detected && fatfsIsMount();
  if(mounted!=app.mounted || detected!=app.detected) {
    app.mounted=mounted; app.detected=detected; app.dirty=true;
    /* Keep the result visible even when the card was removed during a job. */
    if(app.screen!=RESULT)scan();
    if(app.screen==CONFIRM)app.screen=FILES;
    app.confirm_armed=false; app.hold_start=0;
  }
  uint32_t buttons=buttonGetData(), pressed=buttons & ~app.last_buttons;
  app.last_buttons=buttons;
  if(app.screen==HOME) {
    if(pressed & (1U<<BTN_UP)) { app.home_selected=(app.home_selected+2)%3; app.dirty=true; }
    if(pressed & (1U<<BTN_DN)) { app.home_selected=(app.home_selected+1)%3; app.dirty=true; }
    if(pressed & (1U<<BTN_SEL)) {
      app.erase_job=app.home_selected==1;
      app.screen=app.home_selected==0?FILES:app.home_selected==1?ERASE_CONFIRM:INFO;
      app.confirm_armed=false; app.hold_start=0;
      if(app.screen==FILES)scan();
      app.dirty=true;
    }
  } else if(app.screen==INFO) {
    if(pressed & (1U<<BTN_M)) { app.screen=HOME; app.dirty=true; }
  } else if(app.screen==FILES) {
    if(pressed & (1U<<BTN_M)) { app.screen=HOME; app.dirty=true; }
    else if(app.mounted && app.count) {
      if(pressed & (1U<<BTN_UP)) { app.selected=(app.selected+app.count-1)%app.count; app.dirty=true; }
      if(pressed & (1U<<BTN_DN)) { app.selected=(app.selected+1)%app.count; app.dirty=true; }
      if(pressed & (1U<<BTN_SEL)) {
        app.stage="OPEN PROJECT"; app.error[0]=0;
        memset(&app.result,0,sizeof(app.result));
        bool ok=manifest(app.ids[app.selected]);
        if(ok) {
          char path[128]; FILINFO info;
          snprintf(path,sizeof(path),"/prog/fw/%s/%s",app.ids[app.selected],app.manifest.image);
          FRESULT e=f_stat(path,&info);
          if(e!=FR_OK)ok=ioFail(path,e);
          else if((info.fattrib&AM_DIR) || info.fsize<8 || info.fsize>0x100000)ok=fail("Invalid image size");
          else app.image_size=(uint32_t)info.fsize;
        }
        app.screen=ok?CONFIRM:RESULT;
        app.confirm_armed=false; app.hold_start=0; app.dirty=true;
      }
    }
  } else if(app.screen==CONFIRM || app.screen==ERASE_CONFIRM) {
    if(pressed & (1U<<BTN_M)) { app.screen=app.erase_job?HOME:FILES; app.dirty=true; }
    else if(!app.mounted) { app.confirm_armed=false; app.hold_start=0; }
    else if(!(buttons & (1U<<BTN_SEL))) { app.confirm_armed=true; app.hold_start=0; }
    else if(app.confirm_armed) {
      if(!app.hold_start)app.hold_start=millis();
      if((uint32_t)(millis()-app.hold_start)>=1000)runJob();
    }
  } else if(app.screen==RESULT && (pressed & ((1U<<BTN_SEL)|(1U<<BTN_M)))) {
    app.screen=app.erase_job?HOME:FILES; scan();
  }
  draw();
}
static void cliProg(cli_args_t *args)
{
  if(args->argc==1 && args->isStr(0,"list")) {
    scan();
    if(app.screen==CONFIRM || app.screen==ERASE_CONFIRM || app.screen==RESULT) {
      app.screen=HOME; app.confirm_armed=false;
    }
    for(unsigned i=0;i<app.count;i++)cliPrintf("%s\n",app.ids[i]);
    cliPrintf("%u projects%s\n",app.count,app.more?" (limit 16; more omitted)":"");
    return;
  }
  if(args->argc==2 && args->isStr(0,"check")) {
    cliPrintf("H723 SD file check; no target access.\n");
    bool ok=preflight(args->getStr(1));
    if(!closeImage())ok=false;
    cliPrintf("CHECK    : %s (SD files only; target flash not modified)\n",ok?"OK":"FAILED");
    app.erase_job=false; app.screen=FILES; app.dirty=true;
    return;
  }
  cliPrintf("prog list\nprog check <project>\nUse OLED: UP/DN, SEL, then hold SEL 1s to write.\n");
}
static bool progInit(void)
{
  app.dirty=true; app.stage="IDLE";
  app.last_buttons=buttonGetData();
  app.screen=HOME; app.home_selected=0;
  app.detected=sdIsDetected(); app.mounted=app.detected && fatfsIsMount(); scan();
  return cliAdd("prog",cliProg);
}
MODULE_DEF(prog) { .name="prog", .priority=MODULE_PRI_HIGH, .init=progInit };
