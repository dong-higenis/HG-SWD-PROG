"""Exercise the actual OLED app with mock buttons/files and render its 128x64 screens."""
from pathlib import Path
import argparse
import shutil
import struct
import subprocess
import zlib

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc',default=shutil.which('gcc'))
p.add_argument('--loader',type=Path,default=Path('C:/ST/STM32CubeCLT_1.20.0/STM32CubeProgrammer/bin/FlashLoader/0x483.stldr'))
a=p.parse_args()
work=root/'build-swd/host-tests/prog-ui'
work.mkdir(parents=True,exist_ok=True)
image=bytearray((i*17)&255 for i in range(10003))
struct.pack_into('<II',image,0,0x24050000,0x08000101)
loader=a.loader.read_bytes()
for name,data in {
    'prog/loaders/st/0x483.stldr':loader,
    'prog/fw/h723/app.bin':image,
    'prog/fw/zeta/app.bin':image,
}.items():
    dest=work/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
manifest=(f'name=H723 GFX firmware\ndevice=STM32H72x/STM32H73x\n'
          f'algo=/prog/loaders/st/0x483.stldr\nimage=app.bin @ 0x08000000\n'
          f'crc32=0x{zlib.crc32(image):08X}\nloader_crc32=0x{zlib.crc32(loader):08X}\n')
for name in ['h723','zeta']:(work/f'prog/fw/{name}/fw.txt').write_text(manifest,encoding='ascii')
for slot in range(2): (work/f'prog/auto{slot}.cfg').write_bytes(b'corrupt fixture')
(work/'hw_def.h').write_text('#pragma once\n#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\nuint32_t millis(void);\n',encoding='ascii')
(work/'fatfs.h').write_text(r'''
#pragma once
#include "hw_def.h"
#include <stdio.h>
typedef unsigned UINT;
typedef unsigned FRESULT;
typedef struct { FILE *fp; uint32_t size; } FIL;
typedef struct { unsigned index; } DIR;
typedef struct { char fname[256]; unsigned fattrib; uint32_t fsize; } FILINFO;
#define FR_OK 0
#define FR_INT_ERR 2
#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_ALWAYS 8
#define AM_DIR 16
#define f_size(p) ((p)->size)
bool fatfsIsMount(void);
FRESULT f_open(FIL *,const char *,unsigned);
FRESULT f_read(FIL *,void *,UINT,UINT *);
FRESULT f_write(FIL *,const void *,UINT,UINT *);
FRESULT f_sync(FIL *);
FRESULT f_lseek(FIL *,uint32_t);
FRESULT f_close(FIL *);
FRESULT f_stat(const char *,FILINFO *);
FRESULT f_opendir(DIR *,const char *);
FRESULT f_readdir(DIR *,FILINFO *);
FRESULT f_closedir(DIR *);
''',encoding='ascii')
(work/'module.h').write_text(r'''
#pragma once
#include "hw_def.h"
#include <stdio.h>
typedef struct { const char *name; int priority; bool (*init)(void); } module_t;
#define MODULE_DEF(n) static module_t module_##n =
#define MODULE_PRI_HIGH 1
typedef struct { unsigned argc; bool (*isStr)(uint8_t,const char *); char *(*getStr)(uint8_t); } cli_args_t;
enum { BTN_UP, BTN_DN, BTN_SEL, BTN_M };
enum { black=0,white=65535,LCD_FONT_07x10=0,LCD_FONT_11x18=1,LCD_FONT_16x26=2,_DEF_LED1=0 };
#define LCD_WIDTH 128
#define LCD_HEIGHT 64
void cliPrintf(const char *,...);
bool cliAdd(const char *,void (*)(cli_args_t *));
bool sdIsDetected(void);
bool buttonGetPressed(uint8_t);
uint32_t buttonGetData(void);
void delay(uint32_t);
void ledToggle(int);
void ledActivityPulse(void);
void ledActivityStop(void);
void buzzerBeep(uint16_t);
bool lcdIsInit(void);
void lcdSetFont(int);
void lcdClearBuffer(uint32_t);
void lcdPrintf(int,int,uint16_t,const char *,...);
void lcdDrawFillRect(int,int,int,int,uint16_t);
void lcdDrawRect(int,int,int,int,uint16_t);
void lcdDrawFillRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint32_t);
void lcdDrawRoundRect(int32_t,int32_t,int32_t,int32_t,int32_t,uint32_t);
void lcdDrawPixel(uint16_t,uint16_t,uint32_t);
uint32_t lcdReadPixel(uint16_t,uint16_t);
void lcdUpdateDraw(void);
''',encoding='ascii')
esp_dir = work / 'prog/fw/esp_test'
esp_dir.mkdir(parents=True, exist_ok=True)
esp_bin = bytearray(range(64)); esp_bin[0] = 0xE9; esp_bin[12] = 9; esp_bin[13] = 0
(esp_dir / 'app.bin').write_bytes(esp_bin)
(esp_dir / 'fw.txt').write_text('name=ESP test\ndevice=ESP32-S3\nbaud=115200\nflash_size=0x01000000\n'
    + f'image=app.bin,0x00010000,0x00000040,0x{zlib.crc32(esp_bin):08X}\n', encoding='ascii')
source=work/'test_ui.c' 
source.write_text(r'''
#include "prog.c"
#include "lcd/lcd_fonts.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
static uint32_t ticks,buttons;
static bool mounted,card=true;
static bool show_esp,write_error;
static unsigned probes;
static swd_err_t probe_status=SWD_ERR_NORESP;
static target_chip_t probe_chip=ESP32S3_CHIP;
static int programs,fail_program,erase_jobs,esp_jobs,esp_erase_jobs;
static uint8_t pixels[64][128];
static const char *base;
static const lcd_font_t *active_font=&font_07x10;
uint32_t millis(void) { return ticks; }
void delay(uint32_t n) { ticks+=n; }
void ledToggle(int n) { (void)n; }
void ledActivityPulse(void) {}
void ledActivityStop(void) {}
void buzzerBeep(uint16_t ms) {(void)ms;}
uint32_t buttonGetData(void) { return buttons; }
bool buttonGetPressed(uint8_t n) { return (buttons&(1U<<n))!=0; }
bool sdIsDetected(void) { return card; }
bool fatfsIsMount(void) { return mounted; }
void cliPrintf(const char *s,...) { (void)s; }
bool cliAdd(const char *s,void (*f)(cli_args_t *)) { (void)s;(void)f;return true; }
bool lcdIsInit(void) { return true; }
void lcdSetFont(int n) { assert(n>=LCD_FONT_07x10 && n<=LCD_FONT_16x26); active_font=n==LCD_FONT_07x10?&font_07x10:n==LCD_FONT_11x18?&font_11x18:&font_16x26; }
void lcdClearBuffer(uint32_t c) { memset(pixels,c?255:0,sizeof(pixels)); }
void lcdPrintf(int x,int y,uint16_t color,const char *fmt,...) {
  char text[256];va_list args;va_start(args,fmt);vsnprintf(text,sizeof(text),fmt,args);va_end(args);
  assert(x>=0 && y>=0 && x+(int)strlen(text)*active_font->width<=128 && y+active_font->height<=64);
  for(unsigned n=0;text[n];n++) {
    unsigned ch=(unsigned char)text[n];assert(ch>=32 && ch<=126);
    for(int row=0;row<active_font->height;row++)for(int col=0;col<active_font->width;col++)
      if((active_font->data[(ch-32)*active_font->height+row]<<col)&0x8000)pixels[y+row][x+n*active_font->width+col]=color?255:0;
  }
}
void lcdDrawFillRect(int x,int y,int w,int h,uint16_t c) {
  assert(x>=0&&y>=0&&w>=0&&h>=0&&x+w<=128&&y+h<=64);
  for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)pixels[j][i]=c?255:0;
}
void lcdDrawRect(int x,int y,int w,int h,uint16_t c) {
  lcdDrawFillRect(x,y,w,1,c);lcdDrawFillRect(x,y+h-1,w,1,c);
  lcdDrawFillRect(x,y,1,h,c);lcdDrawFillRect(x+w-1,y,1,h,c);
}
void lcdDrawFillRoundRect(int32_t x,int32_t y,int32_t w,int32_t h,int32_t r,uint32_t c) { (void)r;lcdDrawFillRect(x,y,w,h,(uint16_t)c); }
void lcdDrawRoundRect(int32_t x,int32_t y,int32_t w,int32_t h,int32_t r,uint32_t c) { (void)r;lcdDrawRect(x,y,w,h,(uint16_t)c); }
void lcdDrawPixel(uint16_t x,uint16_t y,uint32_t c) { if(x<128&&y<64)pixels[y][x]=c?255:0; }
uint32_t lcdReadPixel(uint16_t x,uint16_t y) { assert(x<128&&y<64);return pixels[y][x]?white:black; }
void lcdUpdateDraw(void) {}
static void capture(const char *name) {
  /* Snapshot the resting pose: skip the entry animation and any selection slide. */
  app.anim_start=ticks-3600;app.sel_start=ticks-SLIDE_MS;app.dirty=true;progDraw();
  char path[512];snprintf(path,sizeof(path),"%s/%s.pgm",base,name);
  FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P5\n128 64\n255\n");
  assert(fwrite(pixels,1,sizeof(pixels),f)==sizeof(pixels));fclose(f);
}
FRESULT f_open(FIL *f,const char *name,unsigned mode) {
  (void)mode;
  if(app.erase_job && app.screen==RUNNING)assert(!progIsEspErase());
  if(app.erase_job && app.screen==RUNNING)assert(!strcmp(name,"/prog/loaders/st/0x483.stldr"));
  char path[512];snprintf(path,sizeof(path),"%s%s",base,name);
  f->fp=fopen(path,(mode&FA_WRITE)?"wb":"rb");if(!f->fp)return 4;
  fseek(f->fp,0,SEEK_END);f->size=(uint32_t)ftell(f->fp);rewind(f->fp);return 0;
}
FRESULT f_read(FIL *f,void *p,UINT n,UINT *got) { *got=(UINT)fread(p,1,n,f->fp);return ferror(f->fp)?1:0; }
FRESULT f_write(FIL *f,const void *p,UINT n,UINT *got) { *got=(UINT)fwrite(p,1,write_error?n/2:n,f->fp);return write_error?1:0; }
FRESULT f_sync(FIL *f) {return fflush(f->fp)?1:0;}
FRESULT f_lseek(FIL *f,uint32_t p) { return fseek(f->fp,p,SEEK_SET)?1:0; }
FRESULT f_close(FIL *f) { return fclose(f->fp)?1:0; }
FRESULT f_stat(const char *p,FILINFO *i) { FIL f;FRESULT e=f_open(&f,p,1);if(e)return e;i->fsize=f.size;i->fattrib=0;return f_close(&f); }
FRESULT f_opendir(DIR *d,const char *p) { assert(!strcmp(p,"/prog/fw"));d->index=0;return 0; }
FRESULT f_readdir(DIR *d,FILINFO *i) {
  const char *names[]={"zeta","h723","../escape",""};
  const char *with_esp[]={"zeta","h723","esp_test",""};
  strcpy(i->fname,show_esp?with_esp[d->index++]:names[d->index++]);i->fattrib=AM_DIR;return 0;
}
FRESULT f_closedir(DIR *d) { (void)d;return 0; }
const char *swdErrStr(swd_err_t e) { return e==SWD_OK?"OK":"data mismatch"; }
swd_err_t swdProgramH723(const swd_program_input_t *in,swd_program_result_t *r) {
  programs++;assert(in->code_size==2020 && in->image_size==10003);
  assert(app.screen==RUNNING && app.image_open);
  ticks+=200;in->progress(in->context,"WRITE",5000,in->image_size);capture("write");
  uint8_t data[8];assert(in->read(in->context,0,data,8));assert(data[3]==0x24);
  memset(r,0,sizeof(*r));r->stage=fail_program?"VERIFY":"DONE";
  r->written=r->verified=in->image_size;r->started=!fail_program;
  r->halted_on_error=fail_program!=0;
  return fail_program?SWD_ERR_VERIFY:SWD_OK;
}
swd_err_t swdEraseH723(const swd_program_input_t *in,swd_program_result_t *r) {
  erase_jobs++;assert(in->code_size==2020 && !in->read && !in->image_size && !in->image_crc);
  assert(app.screen==RUNNING && !app.image_open);
  ticks+=200;in->progress(in->context,"ERASE",4,8);capture("erase-running");
  ticks+=200;in->progress(in->context,"BLANK CHECK",0x80000,0x100000);capture("erase-verify");
  memset(r,0,sizeof(*r));r->stage="DONE";r->verified=0x100000;
  r->erased=r->erase_complete=true;r->elapsed_ms=8000;
  return SWD_OK;
}
bool espProgramRun(const esp_project_t *p,const esp_program_input_t *in,esp_program_result_t *r) {
  esp_jobs++;assert(p->chip==ESP_PROJECT_S3 && p->count==1 && p->total==64);
  uint8_t data[64];assert(in->read(in->context,0,0,data,64));assert(data[0]==0xE9);
  assert(!app.image_open);
  in->progress(in->context,"ESP VERIFY MD5",64,64);
  memset(r,0,sizeof(*r));r->stage="DONE";r->written=r->verified=64;r->started=true;
  return true;
}
bool espProgramErase(esp_project_chip_t chip,const esp_program_input_t *in,esp_program_result_t *r) {
  esp_erase_jobs++;assert(chip==ESP_PROJECT_S3 && !in->read && !card && !app.image_open);
  assert(in->progress(in->context,"ESP ERASE ALL",0,0x1000000));
  ticks+=200;assert(in->progress(in->context,"ESP BLANK CHECK",0x800000,0x1000000));capture("esp-erase-running");
  memset(r,0,sizeof(*r));r->stage="DONE";r->verified=0x1000000;r->erase_complete=true;r->elapsed_ms=40000;
  return true;
}
bool espProgramProbe(esp_program_result_t *r) { probes++;memset(r,0,sizeof(*r));r->stage="PROBE";r->detected=probe_chip;return true; }
swd_err_t swdProbeH723(uint32_t khz,swd_probe_t *r) {
  assert(khz==100);memset(r,0,sizeof(*r));if(probe_status!=SWD_ERR_NORESP)r->dpidr=0x6BA02477;
  return probe_status;
}
static void update(uint32_t keys,uint32_t ms) { ticks+=ms;buttons=keys;progUpdate(); }
static void press(uint32_t keys) { update(0,10);update(keys,10); }
static void confirmWrite(void) { press(1U<<BTN_SEL);update(0,10);update(1U<<BTN_SEL,10);update(1U<<BTN_SEL,1001); }
int main(int argc,char **argv) {
  prog_auto_config_t cfg={0},decoded;
  strcpy(cfg.project[0],"h723");strcpy(cfg.project[2],"esp_test");cfg.sequence=UINT32_MAX;
  uint8_t record[PROG_AUTO_RECORD_SIZE];progAutoEncode(&cfg,record);assert(progAutoDecode(record,&decoded));
  assert(decoded.sequence==UINT32_MAX&&!strcmp(decoded.project[2],"esp_test"));
  for(unsigned i=0;i<sizeof(record);i++) {record[i]^=1;assert(!progAutoDecode(record,&decoded));record[i]^=1;}
  assert(progAutoNewer(0,UINT32_MAX)&&!progAutoNewer(UINT32_MAX,0)&&!progAutoNewer(1,1));
  strcpy(cfg.project[0],"../bad");progAutoEncode(&cfg,record);assert(!progAutoDecode(record,&decoded));
  assert(argc==2);base=argv[1];assert(module_prog.init());update(0,10);
  assert(app.screen==HOME&&!app.count);capture("sd-not-ready-home");
  card=false;update(0,10);assert(app.screen==HOME);capture("home-no-card");
  press(1U<<BTN_SEL);assert(app.screen==HOME && probes==0); // No SD: Start is inert.
  press(1U<<BTN_M);assert(app.screen==MENU);
  press(1U<<BTN_DN);press(1U<<BTN_DN);press(1U<<BTN_DN);assert(app.menu_selected==3);
  press(1U<<BTN_SEL);assert(app.screen==INFO);capture("info");
  mounted=card=true;update(0,10);assert(app.screen==INFO); // SD insertion keeps Info open.
  press(1U<<BTN_M);assert(app.screen==MENU && app.menu_selected==3);
  press(1U<<BTN_DN);press(1U<<BTN_DN);assert(app.menu_selected==1);capture("menu");
  press(1U<<BTN_M);assert(app.screen==HOME);capture("home-setup"); // Card without Auto bindings.
  press(1U<<BTN_SEL);assert(app.screen==AUTO_SETUP && app.auto_selected==0 && probes==0);
  capture("auto-setup");press(1U<<BTN_M);assert(app.screen==MENU);
  press(1U<<BTN_SEL);assert(app.screen==FILES);
  mounted=card=false;update(0,10);assert(app.screen==FILES&&!app.count);capture("no-card");
  press(1U<<BTN_SEL);assert(app.screen==FILES && programs==0);
  card=true;update(0,10);assert(app.screen==FILES&&!app.count);capture("sd-not-ready");
  mounted=true;update(0,10);assert(app.count==2 && !strcmp(app.ids[0],"h723"));capture("files");
  press(1U<<BTN_M);assert(app.screen==MENU);
  press(1U<<BTN_SEL);assert(app.screen==FILES);
  press(1U<<BTN_UP);assert(app.selected==1);press(1U<<BTN_DN);assert(app.selected==0);
  press(1U<<BTN_SEL);assert(app.screen==CONFIRM);capture("confirm");
  update(1U<<BTN_SEL,2000);assert(programs==0); // Opening press cannot confirm.
  update(0,10);update(1U<<BTN_SEL,10);update(1U<<BTN_SEL,999);assert(programs==0);
  update(1U<<BTN_SEL,2);assert(programs==1 && app.screen==RESULT && !app.error[0]);capture("done");
  update(1U<<BTN_SEL,2000);assert(programs==1 && app.screen==RESULT);
  press(1U<<BTN_M);assert(app.screen==FILES);
  press(1U<<BTN_SEL);press(1U<<BTN_M);assert(programs==1 && app.screen==FILES);
  fail_program=1;confirmWrite();assert(programs==2 && app.screen==RESULT && app.error[0]);capture("failure");
  press(1U<<BTN_M);
  char path[512];snprintf(path,sizeof(path),"%s/prog/fw/h723/app.bin",base);
  FILE *f=fopen(path,"r+b");assert(f);fseek(f,9000,SEEK_SET);fputc(0xff,f);fclose(f);
  confirmWrite();assert(programs==2 && app.screen==RESULT && strstr(app.error,"CRC"));
  press(1U<<BTN_M);press(1U<<BTN_SEL);assert(app.screen==CONFIRM);
  mounted=card=false;update(1U<<BTN_SEL,2000);assert(programs==2 && app.screen==FILES && !app.count);
  assert(!app.image_open);
  press(1U<<BTN_M);assert(app.screen==MENU);
  press(1U<<BTN_DN);press(1U<<BTN_DN);press(1U<<BTN_SEL);assert(app.screen==INFO); // Info also works without SD.
  assert(programs==2);
  press(1U<<BTN_M);press(1U<<BTN_UP);assert(app.menu_selected==2);capture("menu-erase");
  press(1U<<BTN_SEL);assert(app.screen==ERASE_TARGET);capture("erase-targets");
  press(1U<<BTN_SEL);assert(app.screen==ERASE_CONFIRM);capture("erase-no-card");
  update(1U<<BTN_SEL,2000);assert(erase_jobs==0);
  mounted=card=true;update(1U<<BTN_SEL,2000);assert(erase_jobs==0);
  capture("erase-confirm");
  update(0,10);update(1U<<BTN_SEL,10);update(1U<<BTN_SEL,999);assert(erase_jobs==0);
  update(1U<<BTN_SEL,2);assert(erase_jobs==1 && programs==2 && app.screen==RESULT && !app.error[0]);
  assert(app.result.erase_complete && !app.result.started);capture("erase-done");
  press(1U<<BTN_M);assert(app.screen==MENU && app.menu_selected==2);
  press(1U<<BTN_SEL);press(1U<<BTN_M);assert(app.screen==MENU && erase_jobs==1);
  snprintf(path,sizeof(path),"%s/prog/loaders/st/0x483.stldr",base);
  f=fopen(path,"r+b");assert(f);fputc(0,f);fclose(f);
  press(1U<<BTN_SEL);assert(app.screen==ERASE_TARGET);
  confirmWrite();assert(erase_jobs==1 && app.screen==RESULT && app.error[0]);capture("erase-failed");
  mounted=card=false;update(0,10);assert(app.screen==RESULT); // Keep failure visible.
  press(1U<<BTN_M);assert(app.screen==MENU);
  press(1U<<BTN_SEL);assert(app.screen==ERASE_TARGET);
  press(1U<<BTN_UP);assert(app.erase_target==3);capture("erase-targets-scroll");
  press(1U<<BTN_UP);assert(app.erase_target==2);
  press(1U<<BTN_SEL);assert(app.screen==ERASE_CONFIRM);capture("esp-erase-confirm");
  update(1U<<BTN_SEL,2000);assert(!esp_erase_jobs);
  press(1U<<BTN_M);assert(app.screen==ERASE_TARGET && !esp_erase_jobs);
  confirmWrite();assert(esp_erase_jobs==1&&erase_jobs==1&&app.screen==RESULT&&!app.error[0]);
  assert(app.result.erase_complete && app.result.verified==0x1000000 && !app.result.started);
  capture("esp-erase-done");
  update(1U<<BTN_SEL,2000);assert(esp_erase_jobs==1);
  press(1U<<BTN_M);assert(app.screen==MENU);
  app.erase_job=false;
  app.screen=FILES;app.mounted=true;app.count=16;app.selected=15;
  for(unsigned i=0;i<16;i++)snprintf(app.ids[i],ID_SIZE,"firmware_release_%02u",i);
  app.dirty=true;progDraw();capture("files-scroll");
  app.screen=RUNNING;app.stage="VERIFY";app.done=app.total=100;app.dirty=true;
  progDraw();capture("verify-100");
  app.screen=RESULT;app.error[0]=0;app.result.verified=887376;app.result.elapsed_ms=24000;
  app.dirty=true;progDraw();capture("done");
  mounted=card=app.mounted=app.detected=true;
  app.screen=FILES;app.count=1;app.selected=0;app.erase_job=false;
  strcpy(app.ids[0],"esp_test");
  press(1U<<BTN_SEL);assert(app.screen==CONFIRM && app.esp_project && app.image_size==64);
  capture("esp-confirm");update(1U<<BTN_SEL,2000);assert(!esp_jobs);
  update(0,10);update(1U<<BTN_SEL,10);update(1U<<BTN_SEL,1001);
  assert(esp_jobs==1 && app.screen==RESULT && !app.error[0] && app.result.verified==64);
  capture("esp-done");
  snprintf(path,sizeof(path),"%s/prog/fw/esp_test/app.bin",base);
  f=fopen(path,"r+b");assert(f);fputc(0x10,f);fclose(f);
  app.screen=FILES;app.count=1;app.selected=0;strcpy(app.ids[0],"esp_test");
  confirmWrite();assert(esp_jobs==1 && app.screen==RESULT && strstr(app.error,"CRC"));
  assert(!app.image_open);
  assert(progLoadManifest("h723") && !app.esp_project);
  /* Restore the fixture image for Auto programming. */
  snprintf(path,sizeof(path),"%s/prog/fw/esp_test/app.bin",base);
  f=fopen(path,"r+b");assert(f);fputc(0xE9,f);fclose(f);
  show_esp=true;app.auto_job=false;app.screen=MENU;app.menu_selected=0;
  app.dirty=true;progDraw();capture("menu-auto");
  press(1U<<BTN_SEL);assert(app.screen==AUTO_SETUP);
  /* Select S3, only the S3 project is offered, save and reload the SD binding. */
  press(1U<<BTN_DN);press(1U<<BTN_DN);press(1U<<BTN_DN);
  assert(app.auto_selected==3);capture("auto-setup-scroll");
  press(1U<<BTN_UP);assert(app.auto_selected==2);
  press(1U<<BTN_SEL);assert(app.screen==AUTO_FILES&&app.count==1&&app.auto_target==2);
  press(1U<<BTN_DN);capture("auto-projects");
  press(1U<<BTN_SEL);assert(app.screen==AUTO_SETUP&&!strcmp(app.auto_config.project[2],"esp_test"));
  capture("auto-setup-saved");
  memset(&app.auto_config,0,sizeof(app.auto_config));progAutoLoad();assert(!strcmp(app.auto_config.project[2],"esp_test"));
  /* Interrupted inactive-slot write keeps the last good binding on reload. */
  write_error=true;assert(!progAutoSave("other"));write_error=false;progAutoLoad();
  assert(!strcmp(app.auto_config.project[2],"esp_test"));
  /* SD removal clears bindings; inserting the same card restores them. */
  mounted=card=false;update(0,10);assert(!app.auto_config.project[2][0]);
  mounted=card=true;update(0,10);assert(!strcmp(app.auto_config.project[2],"esp_test"));
  press(1U<<BTN_M);press(1U<<BTN_M);assert(app.screen==HOME&&esp_jobs==1&&!probes);capture("home");
  press(1U<<BTN_SEL);assert(app.screen==RESULT&&app.auto_job&&!app.error[0]&&esp_jobs==2&&probes==1);
  capture("auto-done");update(1U<<BTN_SEL,2000);assert(esp_jobs==2); /* No repeat on hold. */
  press(1U<<BTN_SEL);assert(app.screen==HOME&&!app.auto_job&&esp_jobs==2); /* PASS: SEL returns home. */
  press(1U<<BTN_SEL);assert(esp_jobs==3&&probes==2); /* Next unit starts from home. */
  probe_status=SWD_ERR_MISMATCH;press(1U<<BTN_SEL);assert(app.screen==HOME);press(1U<<BTN_SEL);
  assert(probes==2&&esp_jobs==3&&strstr(app.error,"Unsupported SWD"));capture("auto-unsupported");
  probe_status=SWD_ERR_NORESP;probe_chip=ESP32C3_CHIP;press(1U<<BTN_SEL);
  assert(esp_jobs==3&&strstr(app.error,"Set Auto project"));capture("auto-unassigned");
  strcpy(app.auto_config.project[3],"esp_test");press(1U<<BTN_SEL);
  assert(esp_jobs==3&&strstr(app.error,"Auto chip mismatch"));
  probe_chip=ESP_UNKNOWN_CHIP;press(1U<<BTN_SEL);assert(esp_jobs==3&&strstr(app.error,"Unsupported ESP"));
  /* Retry with the card pulled fails before any probe; the failure stays on screen. */
  mounted=card=false;update(0,10);unsigned before=probes;press(1U<<BTN_SEL);
  assert(probes==before&&esp_jobs==3&&strstr(app.error,"SD not mounted"));
  capture("auto-failed");
  mounted=card=true;update(0,10);assert(app.screen==RESULT);
  /* H723 Auto uses SWD only and still passes the normal preflight. */
  snprintf(path,sizeof(path),"%s/prog/loaders/st/0x483.stldr",base);
  f=fopen(path,"r+b");assert(f);fputc(0x7f,f);fclose(f);
  snprintf(path,sizeof(path),"%s/prog/fw/h723/app.bin",base);
  f=fopen(path,"r+b");assert(f);fseek(f,9000,SEEK_SET);fputc((9000*17)&255,f);fclose(f);
  strcpy(app.auto_config.project[0],"h723");probe_status=SWD_OK;fail_program=0;
  unsigned before_stm=probes;press(1U<<BTN_SEL);
  assert(programs==3&&probes==before_stm&&!app.error[0]&&app.result.started);
  capture("auto-done-h723");
  press(1U<<BTN_SEL);assert(app.screen==HOME&&!app.auto_job&&programs==3);capture("home-after-auto");
  mounted=card=false;update(0,10);before=probes;
  press(1U<<BTN_SEL);assert(app.screen==HOME&&probes==before); /* Start needs a mounted card. */
  mounted=card=true;update(0,10);assert(app.screen==HOME&&progAutoAssigned());capture("home-ready");
  puts("PASS: Auto home start/setup/no-card gates, menu round trips;");
  puts("PASS: Auto filtering, save/readback, interrupted-write recovery, card swap, detect/dispatch, no held-key repeat,");
  puts("      PASS returns home, next unit from home, unsupported SWD never tries ESP, unknown/unassigned/mismatched/SD-missing gates.");
  puts("PASS: ESP confirmation, dispatch, CRC gate, file closure and STM32 return");
  puts("PASS: Erase release-before-hold, SD insertion while held, loader-only path, invalid loader gate,");
  puts("      erase result/back navigation; home/Flash/Erase/Info navigation, SD missing/not-ready/insertion, project discovery,");
  puts("      release-before-hold confirmation, back, card removal,");
  puts("      CRC refusal before target access, success/failure display, 128x64 text bounds and snapshots");
}
''',encoding='utf-8')
exe=work/'test_ui.exe'
subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror','-I'+str(work),
                '-I'+str(root/'src/ap/modules/prog'),'-I'+str(root/'src/hw/driver'),
                '-I'+str(root/'src/common/hw/include'),'-I'+str(root/'src/lib/esp-serial-flasher/include'),str(source),
                str(root/'src/ap/modules/prog/esp_project.c'),str(root/'src/ap/modules/prog/prog_auto.c'),
                str(root/'src/ap/modules/prog/prog_inspect.c'),str(root/'src/hw/driver/lcd/lcd_fonts.c'),
                '-lm','-o',str(exe)],check=True)
subprocess.run([str(exe),str(work)],check=True)
print(f'OLED snapshots: {work}')
