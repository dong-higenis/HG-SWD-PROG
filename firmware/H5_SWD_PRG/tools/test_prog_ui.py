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
#define AM_DIR 16
#define f_size(p) ((p)->size)
bool fatfsIsMount(void);
FRESULT f_open(FIL *,const char *,unsigned);
FRESULT f_read(FIL *,void *,UINT,UINT *);
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
enum { black=0,white=65535,LCD_FONT_07x10=0,LCD_FONT_11x18=1,_DEF_LED1=0 };
void cliPrintf(const char *,...);
bool cliAdd(const char *,void (*)(cli_args_t *));
bool sdIsDetected(void);
bool buttonGetPressed(uint8_t);
uint32_t buttonGetData(void);
void delay(uint32_t);
void ledToggle(int);
void ledActivityPulse(void);
void ledActivityStop(void);
bool lcdIsInit(void);
void lcdSetFont(int);
void lcdClearBuffer(uint32_t);
void lcdPrintf(int,int,uint16_t,const char *,...);
void lcdDrawFillRect(int,int,int,int,uint16_t);
void lcdDrawRect(int,int,int,int,uint16_t);
void lcdUpdateDraw(void);
''',encoding='ascii')
source=work/'test_ui.c'
source.write_text(r'''
#include "prog.c"
#include "lcd/lcd_fonts.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
static uint32_t ticks,buttons;
static bool mounted,card=true;
static int programs,fail_program,erase_jobs;
static uint8_t pixels[64][128];
static const char *base;
static const lcd_font_t *active_font=&font_07x10;
uint32_t millis(void) { return ticks; }
void delay(uint32_t n) { ticks+=n; }
void ledToggle(int n) { (void)n; }
void ledActivityPulse(void) {}
void ledActivityStop(void) {}
uint32_t buttonGetData(void) { return buttons; }
bool buttonGetPressed(uint8_t n) { return (buttons&(1U<<n))!=0; }
bool sdIsDetected(void) { return card; }
bool fatfsIsMount(void) { return mounted; }
void cliPrintf(const char *s,...) { (void)s; }
bool cliAdd(const char *s,void (*f)(cli_args_t *)) { (void)s;(void)f;return true; }
bool lcdIsInit(void) { return true; }
void lcdSetFont(int n) { assert(n==LCD_FONT_07x10 || n==LCD_FONT_11x18); active_font=n==LCD_FONT_07x10?&font_07x10:&font_11x18; }
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
void lcdUpdateDraw(void) {}
static void capture(const char *name) {
  char path[512];snprintf(path,sizeof(path),"%s/%s.pgm",base,name);
  FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P5\n128 64\n255\n");
  assert(fwrite(pixels,1,sizeof(pixels),f)==sizeof(pixels));fclose(f);
}
FRESULT f_open(FIL *f,const char *name,unsigned mode) {
  (void)mode;
  if(app.erase_job && app.screen==RUNNING)assert(!strcmp(name,"/prog/loaders/st/0x483.stldr"));
  char path[512];snprintf(path,sizeof(path),"%s%s",base,name);
  f->fp=fopen(path,"rb");if(!f->fp)return 4;
  fseek(f->fp,0,SEEK_END);f->size=(uint32_t)ftell(f->fp);rewind(f->fp);return 0;
}
FRESULT f_read(FIL *f,void *p,UINT n,UINT *got) { *got=(UINT)fread(p,1,n,f->fp);return ferror(f->fp)?1:0; }
FRESULT f_lseek(FIL *f,uint32_t p) { return fseek(f->fp,p,SEEK_SET)?1:0; }
FRESULT f_close(FIL *f) { return fclose(f->fp)?1:0; }
FRESULT f_stat(const char *p,FILINFO *i) { FIL f;FRESULT e=f_open(&f,p,1);if(e)return e;i->fsize=f.size;i->fattrib=0;return f_close(&f); }
FRESULT f_opendir(DIR *d,const char *p) { assert(!strcmp(p,"/prog/fw"));d->index=0;return 0; }
FRESULT f_readdir(DIR *d,FILINFO *i) {
  const char *names[]={"zeta","h723","../escape",""};
  strcpy(i->fname,names[d->index++]);i->fattrib=AM_DIR;return 0;
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
static void update(uint32_t keys,uint32_t ms) { ticks+=ms;buttons=keys;progUpdate(); }
static void press(uint32_t keys) { update(0,10);update(keys,10); }
static void confirmWrite(void) { press(1U<<BTN_SEL);update(0,10);update(1U<<BTN_SEL,10);update(1U<<BTN_SEL,1001); }
int main(int argc,char **argv) {
  assert(argc==2);base=argv[1];assert(module_prog.init());update(0,10);
  assert(app.screen==HOME&&!app.count);capture("sd-not-ready-home");
  card=false;update(0,10);assert(app.screen==HOME);capture("home-no-card");
  press(1U<<BTN_UP);assert(app.home_selected==2);
  press(1U<<BTN_SEL);assert(app.screen==INFO);capture("info");
  mounted=card=true;update(0,10);assert(app.screen==INFO); // SD insertion keeps Info open.
  press(1U<<BTN_M);assert(app.screen==HOME && app.home_selected==2);
  press(1U<<BTN_DN);assert(app.home_selected==0);capture("home");
  press(1U<<BTN_SEL);assert(app.screen==FILES);
  mounted=card=false;update(0,10);assert(app.screen==FILES&&!app.count);capture("no-card");
  press(1U<<BTN_SEL);assert(app.screen==FILES && programs==0);
  card=true;update(0,10);assert(app.screen==FILES&&!app.count);capture("sd-not-ready");
  mounted=true;update(0,10);assert(app.count==2 && !strcmp(app.ids[0],"h723"));capture("files");
  press(1U<<BTN_M);assert(app.screen==HOME);
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
  press(1U<<BTN_M);assert(app.screen==HOME);
  press(1U<<BTN_UP);press(1U<<BTN_SEL);assert(app.screen==INFO); // Info also works without SD.
  assert(programs==2);
  press(1U<<BTN_M);press(1U<<BTN_UP);assert(app.home_selected==1);capture("home-erase");
  press(1U<<BTN_SEL);assert(app.screen==ERASE_CONFIRM);capture("erase-no-card");
  update(1U<<BTN_SEL,2000);assert(erase_jobs==0);
  mounted=card=true;update(1U<<BTN_SEL,2000);assert(erase_jobs==0);
  capture("erase-confirm");
  update(0,10);update(1U<<BTN_SEL,10);update(1U<<BTN_SEL,999);assert(erase_jobs==0);
  update(1U<<BTN_SEL,2);assert(erase_jobs==1 && programs==2 && app.screen==RESULT && !app.error[0]);
  assert(app.result.erase_complete && !app.result.started);capture("erase-done");
  press(1U<<BTN_M);assert(app.screen==HOME && app.home_selected==1);
  press(1U<<BTN_SEL);press(1U<<BTN_M);assert(app.screen==HOME && erase_jobs==1);
  snprintf(path,sizeof(path),"%s/prog/loaders/st/0x483.stldr",base);
  f=fopen(path,"r+b");assert(f);fputc(0,f);fclose(f);
  confirmWrite();assert(erase_jobs==1 && app.screen==RESULT && app.error[0]);capture("erase-failed");
  mounted=card=false;update(0,10);assert(app.screen==RESULT); // Keep failure visible.
  press(1U<<BTN_M);assert(app.screen==HOME);
  app.erase_job=false;
  app.screen=FILES;app.mounted=true;app.count=16;app.selected=15;
  for(unsigned i=0;i<16;i++)snprintf(app.ids[i],ID_SIZE,"firmware_release_%02u",i);
  app.dirty=true;progDraw();capture("files-scroll");
  app.screen=RUNNING;app.stage="VERIFY";app.done=app.total=100;app.dirty=true;
  progDraw();capture("verify-100");
  app.screen=RESULT;app.error[0]=0;app.result.verified=887376;app.result.elapsed_ms=24000;
  app.dirty=true;progDraw();capture("done");
  puts("PASS: Erase release-before-hold, SD insertion while held, loader-only path, invalid loader gate,");
  puts("      erase result/back navigation; home/Flash/Erase/Info navigation, SD missing/not-ready/insertion, project discovery,");
  puts("      release-before-hold confirmation, back, card removal,");
  puts("      CRC refusal before target access, success/failure display, 128x64 text bounds and snapshots");
}
''',encoding='utf-8')
exe=work/'test_ui.exe'
subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror','-I'+str(work),
                '-I'+str(root/'src/ap/modules/prog'),'-I'+str(root/'src/hw/driver'),
                '-I'+str(root/'src/common/hw/include'),str(source),
                str(root/'src/ap/modules/prog/prog_inspect.c'),str(root/'src/hw/driver/lcd/lcd_fonts.c'),
                '-o',str(exe)],check=True)
subprocess.run([str(exe),str(work)],check=True)
print(f'OLED snapshots: {work}')
