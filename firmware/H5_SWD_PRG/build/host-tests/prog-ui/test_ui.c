
#include "prog.c"
#include "lcd/lcd_fonts.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static uint32_t ticks,buttons;
static bool mounted,card=true;
static bool write_error;
static unsigned probes,unlinks;
static int stop_at_unlink=-1;
static swd_err_t probe_status=SWD_ERR_NORESP;
static target_chip_t probe_chip=ESP32S3_CHIP;
static int programs,fail_program,erase_jobs,esp_jobs,esp_erase_jobs;
static bool gpio_out[3];
static unsigned power_seen; /* Bit 0: the SWD probe saw 3V_OUT on, bit 1: so did the write. */
static uint8_t pixels[64][128];
static const char *base,*shots;
static const lcd_font_t *active_font=&font_07x10;
static void capture(const char *name);
uint32_t millis(void) { return ticks; }
void delay(uint32_t n) { ticks+=n; }
void ledToggle(int n) { (void)n; }
void ledActivityPulse(void) {}
void ledActivityStop(void) {}
void buzzerBeep(uint16_t ms) {(void)ms;}
void gpioPinWrite(uint8_t ch,bool on) { assert(ch==TARGET_3V3||ch==TARGET_5V);gpio_out[ch]=on; }
bool gpioPinRead(uint8_t ch) { return gpio_out[ch]; }
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
  char path[512];snprintf(path,sizeof(path),"%s/%s.pgm",shots,name);
  FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P5\n128 64\n255\n");
  assert(fwrite(pixels,1,sizeof(pixels),f)==sizeof(pixels));fclose(f);
}
/* FatFs on top of the fixture directory: the card root is base. */
void *hostOpenDir(const char *path);
int hostCloseDir(void *dir);
const char *hostReadDir(void *dir);
static const char *host(const char *name,char *out) { snprintf(out,512,"%s%s",base,name);return out; }
static unsigned attributes(const char *path) {
  struct stat st;if(stat(path,&st))return 0;
  return (S_ISDIR(st.st_mode)?AM_DIR:0)|((st.st_mode&S_IWUSR)?0:AM_RDO);
}
FRESULT f_open(FIL *f,const char *name,unsigned mode) {
  if(app.job==JOB_ERASE && app.screen==RUNNING)assert(!progIsEspErase() && !strcmp(name,"/prog/loaders/st/0x483.stldr"));
  char path[512];f->fp=fopen(host(name,path),(mode&FA_WRITE)?"wb":"rb");if(!f->fp)return FR_NO_FILE;
  fseek(f->fp,0,SEEK_END);f->size=(uint32_t)ftell(f->fp);rewind(f->fp);return 0;
}
FRESULT f_read(FIL *f,void *p,UINT n,UINT *got) { *got=(UINT)fread(p,1,n,f->fp);return ferror(f->fp)?1:0; }
FRESULT f_write(FIL *f,const void *p,UINT n,UINT *got) { *got=(UINT)fwrite(p,1,write_error?n/2:n,f->fp);return write_error?1:0; }
FRESULT f_sync(FIL *f) {return fflush(f->fp)?1:0;}
FRESULT f_lseek(FIL *f,uint32_t p) { return fseek(f->fp,p,SEEK_SET)?1:0; }
FRESULT f_close(FIL *f) { return fclose(f->fp)?1:0; }
FRESULT f_stat(const char *p,FILINFO *i) {
  char path[512];struct stat st;if(stat(host(p,path),&st))return FR_NO_FILE;
  i->fsize=(uint32_t)st.st_size;i->fattrib=attributes(path);return 0;
}
FRESULT f_opendir(DIR *d,const char *p) {
  host(p,d->path);d->handle=hostOpenDir(d->path);return d->handle?FR_OK:FR_NO_PATH;
}
FRESULT f_readdir(DIR *d,FILINFO *i) {
  const char *name=hostReadDir(d->handle);
  i->fname[0]=0;if(!name)return FR_OK;
  char path[1024];snprintf(i->fname,sizeof(i->fname),"%s",name);snprintf(path,sizeof(path),"%s/%s",d->path,name);
  i->fattrib=attributes(path);return FR_OK;
}
FRESULT f_closedir(DIR *d) { return hostCloseDir(d->handle)?FR_INT_ERR:FR_OK; }
FRESULT f_unlink(const char *p) {
  char path[512];host(p,path);
  struct stat st;if(stat(path,&st))return FR_NO_FILE; /* As FatFs: a missing file is not an error to retry. */
  if(!unlinks++)capture("wipe-running");
  if((int)unlinks==stop_at_unlink)buttons|=1U<<BTN_M;
  return ((attributes(path)&AM_DIR)?rmdir(path):remove(path))?FR_DENIED:FR_OK;
}
FRESULT f_mkdir(const char *p) { char path[512];return mkdir(host(p,path))?FR_EXIST:FR_OK; }
FRESULT f_rename(const char *a,const char *b) { char x[512],y[512];return rename(host(a,x),host(b,y))?FR_DENIED:FR_OK; }
FRESULT f_chmod(const char *p,unsigned attr,unsigned mask) {
  char path[512];assert(mask==AM_RDO);
  return chmod(host(p,path),(attr&AM_RDO)?S_IREAD:(S_IREAD|S_IWRITE))?FR_DENIED:FR_OK;
}
static unsigned entries(const char *name) {
  char path[512];void *d=hostOpenDir(host(name,path));assert(d);unsigned n=0;
  while(hostReadDir(d))n++;
  hostCloseDir(d);return n;
}
static void touch(const char *name,const char *text) {
  char path[512];FILE *f=fopen(host(name,path),"wb");assert(f);fputs(text,f);fclose(f);
}
static void folder(const char *name) { char path[512];assert(!mkdir(host(name,path))); }
static void patch(const char *name,long offset,int value) {
  char path[512];FILE *f=fopen(host(name,path),"r+b");assert(f);fseek(f,offset,SEEK_SET);fputc(value,f);fclose(f);
}
const char *swdErrStr(swd_err_t e) { return e==SWD_OK?"OK":"data mismatch"; }
swd_err_t swdProgramH723(const swd_program_input_t *in,swd_program_result_t *r) {
  programs++;assert(in->code_size==2020 && in->image_size==10003);
  if(gpio_out[TARGET_3V3])power_seen|=2;
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
  if(gpio_out[TARGET_3V3])power_seen|=1;
  return probe_status;
}
/* ESP and Hub simulated at the net.h/hub.h boundary (their own tests: tools/test_net.py). */
static int status_calls;static bool esp_present=true;static net_info_t esp_info;static int join_polls,scan_polls,offs,settings_saves;
static uint8_t join_final;static char last_pw[80],last_apn[80];static net_store_t settings;
static int hub_fetches,hub_installs;static const char *hub_list_error,*hub_install_error;static char last_install[32];
bool netIsPresent(void) { return esp_present; }
const char *netEspVersion(void) { return "p1 test"; }
uint8_t netInfo(net_info_t *info) {
  status_calls++;
  if(!esp_present)return NET_ST_NO_LINK;
  if(join_polls==2)capture(esp_info.mode==NET_MODE_LTE?"lte-joining":"wifi-joining");
  if(join_polls>0 && --join_polls==0){
    esp_info.state=join_final;
    if(join_final==NET_STATE_FAILED)strcpy(esp_info.detail,"Wrong password");
    else{esp_info.ip[0]=192;esp_info.ip[1]=168;esp_info.ip[3]=7;}
  }
  if(info){*info=esp_info;}
  return NET_ST_OK;
}
bool netIsConnected(void) { return esp_present && esp_info.state==NET_STATE_CONNECTED; }
const net_info_t *netInfoCached(void) { return &esp_info; }
void netSetIdleHook(void (*hook)(void)) { (void)hook; }
uint8_t netOff(void) { offs++;memset(&esp_info,0,sizeof(esp_info));return esp_present?NET_ST_OK:NET_ST_NO_LINK; }
uint8_t netWifiScan(void) { scan_polls=2;return esp_present?NET_ST_OK:NET_ST_NO_LINK; }
uint8_t netWifiList(net_ap_t *list,unsigned max,unsigned *count) {
  static const net_ap_t aps[]={{-48,true,"Lab"},{-60,false,"Cafe"},{-80,true,"Far away network name"}};
  *count=0;if(scan_polls>0){scan_polls--;if(!scan_polls)capture("wifi-scanning");return NET_ST_BUSY;}
  *count=3<max?3:max;memcpy(list,aps,*count*sizeof(*list));return NET_ST_OK;
}
uint8_t netWifiJoin(const char *ssid,const char *pw) {
  memset(&esp_info,0,sizeof(esp_info));esp_info.mode=NET_MODE_WIFI;esp_info.state=NET_STATE_CONNECTING;esp_info.rssi=-50;
  snprintf(esp_info.detail,sizeof(esp_info.detail),"%s",ssid);snprintf(last_pw,sizeof(last_pw),"%s",pw);
  join_polls=3;join_final=!strcmp(ssid,"Cafe")||!strcmp(pw,"secret")?NET_STATE_CONNECTED:NET_STATE_FAILED;return NET_ST_OK;
}
uint8_t netLteJoin(const char *apn) {
  memset(&esp_info,0,sizeof(esp_info));esp_info.mode=NET_MODE_LTE;esp_info.state=NET_STATE_CONNECTING;
  snprintf(esp_info.detail,sizeof(esp_info.detail),"%s",apn);snprintf(last_apn,sizeof(last_apn),"%s",apn);
  join_polls=3;join_final=NET_STATE_CONNECTED;return NET_ST_OK;
}
const char *netStatusText(uint8_t s) { return s==NET_ST_NO_LINK?"No ESP link":s?"Net error":"OK"; }
net_store_t *netSettings(void) { return &settings; }
void netSettingsReload(bool mounted) { (void)mounted; }
bool netSettingsSave(void) { settings_saves++;return true; }
const net_apn_t net_apns[]={{"Global","mobile"},{"KT","lte.ktfwing.com"}};
const unsigned net_apn_count=2;
const char *hubFetchList(hub_list_t *list,hub_progress_t progress,void *context) {
  static const hub_project_t projects[]={
    {"h7","H7 board","stm32h723","2.0.0",PROG_TARGET_H723},{"esp_test","ESP test","esp32s3","1.2.3-rc.1",PROG_TARGET_ESP32S3},
    {"new-s3","New S3","esp32s3","1.0.0",PROG_TARGET_ESP32S3},{"future","Future","stm32h730","0.1.0",PROG_TARGET_NONE}};
  hub_fetches++;assert(progress(context,"HUB LIST",0,1));memset(list,0,sizeof(*list));
  if(hub_list_error)return hub_list_error;
  memcpy(list->projects,projects,sizeof(projects));list->count=4;return NULL;
}
const char *hubInstall(const hub_project_t *project,hub_progress_t progress,void *context) {
  hub_installs++;snprintf(last_install,sizeof(last_install),"%s",project->id);
  assert(app.screen==RUNNING && progress(context,"HUB INFO",0,1));capture("install-wait");
  assert(progress(context,"firmware.bin",400,1000));ticks+=60;assert(progress(context,"firmware.bin",420,1000));
  ticks+=40;assert(progress(context,"firmware.bin",430,1000));capture("installing");
  assert(app.pct_seen==43 && !strcmp(app.stage_name,"firmware.bin"));
  assert(progress(context,"fw.txt",1000,1000));return hub_install_error;
}
static void update(uint32_t keys,uint32_t ms) { ticks+=ms;buttons=keys;progUpdate(); }
static void press(uint32_t keys) { update(0,10);update(keys,10); }
static void confirmWrite(void) { press(1U<<BTN_SEL);update(0,10);update(1U<<BTN_SEL,10);update(1U<<BTN_SEL,1001); }
static void tap(uint32_t keys) { update(0,10);update(keys,10);update(0,10); } /* Keyboard keys act on release. */
static void typeText(const char *text) {
  for(;*text;text++){ while(kbdChar(&app.kbd,0)!=*text)tap(1U<<BTN_DN); tap(1U<<BTN_SEL); }
}
static void kbdMenu(unsigned item) { /* Hold SEL, pick a menu item. */
  update(0,10);update(1U<<BTN_SEL,10);update(1U<<BTN_SEL,MENU_HOLD_MS);assert(app.kbd.menu);update(0,10);
  while(app.kbd.menu_selected!=item)tap(1U<<BTN_DN);
  tap(1U<<BTN_SEL);
}
static unsigned usable(prog_target_t t) { return progCatalogFilter(&app.catalog,t,true,NULL,0); }
static unsigned listed(prog_target_t t) { return progCatalogFilter(&app.catalog,t,false,NULL,0); }
#define SEL (1U<<BTN_SEL)
#define M (1U<<BTN_M)
#define UP (1U<<BTN_UP)
#define DN (1U<<BTN_DN)
int main(int argc,char **argv) {
  prog_auto_config_t cfg={0},decoded;
  strcpy(cfg.project[0],"h723");strcpy(cfg.project[2],"esp_test");cfg.sequence=UINT32_MAX;
  uint8_t record[PROG_AUTO_RECORD_SIZE];progAutoEncode(&cfg,record);assert(progAutoDecode(record,&decoded));
  assert(decoded.sequence==UINT32_MAX&&!strcmp(decoded.project[2],"esp_test"));
  for(unsigned i=0;i<sizeof(record);i++) {record[i]^=1;assert(!progAutoDecode(record,&decoded));record[i]^=1;}
  assert(progAutoNewer(0,UINT32_MAX)&&!progAutoNewer(UINT32_MAX,0)&&!progAutoNewer(1,1));
  strcpy(cfg.project[0],"../bad");progAutoEncode(&cfg,record);assert(!progAutoDecode(record,&decoded));
  /* The chip table follows the Hub registry: device= picks the group, unknown devices stay apart. */
  assert(progTargetFromDevice("STM32H72x/STM32H73x")==PROG_TARGET_H723&&progTargetFromDevice("ESP32-C3")==PROG_TARGET_ESP32C3);
  assert(progTargetFromDevice("RP2040")==PROG_TARGET_NONE&&progTargetFromEsp(ESP_PROJECT_S3)==PROG_TARGET_ESP32S3);
  assert(!strcmp(progTargetInfo(PROG_TARGET_ESP32S3)->chip,"esp32s3")&&!progTargetInfo(PROG_TARGET_NONE));
  assert(progTargetFromChip("stm32h723")==PROG_TARGET_H723&&progTargetFromChip("stm32h730")==PROG_TARGET_NONE);
  assert(argc==3);base=argv[1];shots=argv[2];assert(module_prog.init());update(0,10);
  assert(app.screen==HOME&&!app.catalog.count);capture("sd-not-ready-home");
  card=false;update(0,10);assert(app.screen==HOME);capture("home-no-card");
  press(SEL);assert(app.screen==HOME && probes==0); // No SD: Start is inert.
  press(M);assert(app.screen==MENU);
  press(UP);assert(app.menu_selected==5);
  press(SEL);assert(app.screen==INFO);capture("info");
  mounted=card=true;update(0,10);assert(app.screen==INFO); // SD insertion keeps Info open.
  /* Catalog: packages grouped by their fw.txt device, sorted by name; bad folder names and folders without fw.txt skipped. */
  assert(app.catalog.count==7 && !app.catalog.more && app.loader_present);
  assert(usable(PROG_TARGET_H723)==2 && usable(PROG_TARGET_ESP32S3)==1 && listed(PROG_TARGET_ESP32S3)==2);
  assert(listed(PROG_TARGET_ESP32)==0 && listed(PROG_TARGET_NONE)==3 && !usable(PROG_TARGET_NONE));
  assert(!strcmp(app.catalog.entries[0].id,"h723") && !strcmp(app.catalog.entries[1].id,"zeta"));
  assert(!strcmp(app.catalog.entries[2].id,"broken") && !strcmp(app.catalog.entries[2].error,"ESP image missing"));
  assert(!strcmp(app.catalog.entries[3].name,"ESP test") && app.catalog.entries[3].size==64 && app.catalog.entries[3].images==1);
  assert(!strcmp(app.catalog.entries[4].id,"ghost") && !strcmp(app.catalog.entries[4].error,"Unknown chip"));
  assert(!strcmp(app.catalog.entries[5].id,"mixup") && !strcmp(app.catalog.entries[5].error,"Chip mismatch"));
  assert(app.catalog.entries[6].target==PROG_TARGET_NONE && !strcmp(app.catalog.entries[6].name,"Mystery"));
  assert(!strcmp(app.catalog.entries[6].error,"Unknown device"));
  /* release.txt: version shown only from a valid file; schema 2 and duplicate keys fall back to device=. */
  assert(!strcmp(app.catalog.entries[3].version,"1.2.3-rc.1") && !app.catalog.entries[0].version[0]);
  assert(app.catalog.entries[0].target==PROG_TARGET_H723 && !app.catalog.entries[1].version[0]);
  assert(!app.catalog.entries[2].version[0]);
  assert(app.catalog.entries[0].size==10003 && !app.catalog.entries[0].error);
  press(M);assert(app.screen==MENU && app.menu_selected==5);
  press(UP);press(UP);press(UP);press(UP);assert(app.menu_selected==1);capture("menu");
  press(M);assert(app.screen==HOME);capture("home-setup"); // Card without Auto bindings.
  press(SEL);assert(app.screen==CHIPS && app.pick==PICK_AUTO && app.target==0 && probes==0);
  capture("auto-setup");press(M);assert(app.screen==MENU);
  /* Manual -> Flash -> chip -> package. */
  press(SEL);assert(app.screen==MANUAL);capture("manual");
  press(SEL);assert(app.screen==CHIPS && app.pick==PICK_FLASH);capture("flash-chips");
  mounted=card=false;update(0,10);assert(app.screen==CHIPS&&!app.catalog.count);capture("no-card");
  press(SEL);assert(app.screen==CHIPS && programs==0);
  card=true;update(0,10);assert(app.screen==CHIPS&&!app.catalog.count);capture("sd-not-ready");
  mounted=true;update(0,10);assert(app.catalog.count==7);
  press(SEL);assert(app.screen==PACKAGES && app.view_count==2 && !strcmp(progPackageAt(0)->id,"h723"));capture("files");
  press(M);assert(app.screen==CHIPS);
  press(SEL);assert(app.screen==PACKAGES);
  press(UP);assert(app.selected==1);press(DN);assert(app.selected==0);
  press(SEL);assert(app.screen==CONFIRM && !strcmp(app.job_id,"h723"));capture("confirm");
  update(SEL,2000);assert(programs==0); // Opening press cannot confirm.
  update(0,10);update(SEL,10);update(SEL,999);assert(programs==0);
  update(SEL,2);assert(programs==1 && app.screen==RESULT && !app.error[0]);capture("done");
  update(SEL,2000);assert(programs==1 && app.screen==RESULT);
  press(M);assert(app.screen==PACKAGES && app.selected==0);
  press(SEL);press(M);assert(programs==1 && app.screen==PACKAGES);
  fail_program=1;confirmWrite();assert(programs==2 && app.screen==RESULT && app.error[0]);capture("failure");
  press(M);
  patch("/prog/fw/h723/app.bin",9000,0xff);
  confirmWrite();assert(programs==2 && app.screen==RESULT && strstr(app.error,"CRC"));
  press(M);press(SEL);assert(app.screen==CONFIRM);
  mounted=card=false;update(SEL,2000);assert(programs==2 && app.screen==PACKAGES && !app.view_count);
  assert(!app.image_open);
  press(M);press(M);assert(app.screen==MANUAL);
  press(DN);assert(app.manual_selected==1);capture("manual-erase");
  press(SEL);assert(app.screen==CHIPS && app.pick==PICK_ERASE);capture("erase-targets"); // Works without SD.
  press(SEL);assert(app.screen==ERASE_CONFIRM && app.job==JOB_ERASE);capture("erase-no-card");
  update(SEL,2000);assert(erase_jobs==0);
  mounted=card=true;update(SEL,2000);assert(erase_jobs==0);
  capture("erase-confirm");
  update(0,10);update(SEL,10);update(SEL,999);assert(erase_jobs==0);
  update(SEL,2);assert(erase_jobs==1 && programs==2 && app.screen==RESULT && !app.error[0]);
  assert(app.result.erase_complete && !app.result.started);capture("erase-done");
  press(M);assert(app.screen==CHIPS && app.pick==PICK_ERASE);
  press(SEL);press(M);assert(app.screen==CHIPS && erase_jobs==1);
  patch("/prog/loaders/st/0x483.stldr",0,0);
  confirmWrite();assert(erase_jobs==1 && app.screen==RESULT && app.error[0]);capture("erase-failed");
  mounted=card=false;update(0,10);assert(app.screen==RESULT); // Keep failure visible.
  press(M);assert(app.screen==CHIPS);
  press(UP);assert(app.target==3);capture("erase-targets-scroll");
  press(UP);assert(app.target==2);
  press(SEL);assert(app.screen==ERASE_CONFIRM);capture("esp-erase-confirm");
  update(SEL,2000);assert(!esp_erase_jobs);
  press(M);assert(app.screen==CHIPS && !esp_erase_jobs);
  confirmWrite();assert(esp_erase_jobs==1&&erase_jobs==1&&app.screen==RESULT&&!app.error[0]);
  assert(app.result.erase_complete && app.result.verified==0x1000000 && !app.result.started);
  capture("esp-erase-done");
  update(SEL,2000);assert(esp_erase_jobs==1);
  press(M);assert(app.screen==CHIPS);
  patch("/prog/loaders/st/0x483.stldr",0,loader0());
  patch("/prog/fw/h723/app.bin",9000,(9000*17)&255);
  /* Synthetic frames: a long list and the end of a write. */
  mounted=card=true;update(0,10);
  app.screen=PACKAGES;app.pick=PICK_FLASH;app.view_count=16;app.selected=15;
  for(unsigned i=0;i<16;i++){app.view[i]=0;}
  strcpy(app.catalog.entries[0].name,"firmware_release_with_a_long_name");
  app.dirty=true;progDraw();capture("files-scroll");
  ticks+=SCROLL_MS*8;app.dirty=true;progDraw();assert(app.marquee);
  app.screen=RUNNING;app.stage="VERIFY";app.done=app.total=100;app.dirty=true;
  progDraw();capture("verify-100");
  app.screen=RESULT;app.error[0]=0;app.result.verified=887376;app.result.elapsed_ms=24000;
  app.dirty=true;progDraw();capture("done");
  /* ESP write: the S3 list offers only the package whose BIN files are present. */
  app.screen=CHIPS;app.pick=PICK_ERASE;app.job=JOB_ERASE;progRefresh();
  press(M);press(UP);assert(app.screen==MANUAL && app.manual_selected==0);
  press(SEL);assert(app.screen==CHIPS && app.pick==PICK_FLASH && app.target==2);capture("flash-chips-s3");
  press(SEL);assert(app.screen==PACKAGES && app.view_count==1 && !strcmp(progPackageAt(0)->id,"esp_test"));
  press(SEL);assert(app.screen==CONFIRM && app.package.esp_project && app.image_size==64);
  capture("esp-confirm");update(SEL,2000);assert(!esp_jobs);
  update(0,10);update(SEL,10);update(SEL,1001);
  assert(esp_jobs==1 && app.screen==RESULT && !app.error[0] && app.result.verified==64);
  capture("esp-done");
  patch("/prog/fw/esp_test/app.bin",0,0x10);
  press(M);confirmWrite();assert(esp_jobs==1 && app.screen==RESULT && strstr(app.error,"CRC"));
  assert(!app.image_open);
  assert(progLoadManifest("h723") && !app.package.esp_project && app.package.target==PROG_TARGET_H723);
  assert(!progLoadManifest("mixup") && strstr(app.error,"Chip mismatch")); // Unknown packages cannot be written.
  patch("/prog/fw/esp_test/app.bin",0,0xE9); /* Restore the fixture image for Auto programming. */
  /* Storage -> Installed bin: every package by chip, including ones that cannot be written. */
  press(M);press(M);press(M);assert(app.screen==MANUAL);press(M);assert(app.screen==MENU && app.menu_selected==1);
  press(DN);press(SEL);assert(app.screen==STORAGE);capture("storage");
  press(SEL);assert(app.screen==CHIPS && app.pick==PICK_INSTALLED && progChipRows()==5);capture("installed-chips");
  assert(app.target==2);press(SEL);assert(app.screen==PACKAGES && app.view_count==2);capture("installed-s3");
  assert(!strcmp(progPackageAt(0)->id,"broken"));
  press(SEL);assert(app.screen==DETAILS);capture("installed-details-broken");
  press(DN);assert(app.screen==DETAILS && !strcmp(progPackageAt(app.selected)->id,"esp_test"));capture("installed-details");
  press(M);press(M);press(DN);press(DN);assert(app.target==4);capture("installed-chips-unknown");
  press(SEL);assert(app.screen==PACKAGES && app.view_count==3 && !strcmp(progPackageAt(2)->id,"mystery"));
  press(SEL);assert(app.screen==DETAILS && progPackageAt(0)->error);capture("installed-unknown");
  mounted=card=false;update(0,10);assert(app.screen==PACKAGES && !app.view_count); // Details close with the card.
  mounted=card=true;update(0,10);
  press(M);assert(app.screen==CHIPS);press(M);assert(app.screen==STORAGE);
  press(M);press(UP);assert(app.screen==MENU && app.menu_selected==1);press(UP);
  app.dirty=true;progDraw();capture("menu-auto");
  /* Auto setup: select S3, only the usable S3 package is offered, save and reload the SD binding. */
  press(SEL);assert(app.screen==CHIPS && app.pick==PICK_AUTO);
  app.target=0;press(DN);press(DN);press(DN);
  assert(app.target==3);capture("auto-setup-scroll");
  press(UP);assert(app.target==2);
  press(SEL);assert(app.screen==PACKAGES&&app.view_count==1&&app.selected==0);
  press(DN);capture("auto-projects");
  press(SEL);assert(app.screen==CHIPS&&!strcmp(app.auto_config.project[2],"esp_test"));
  capture("auto-setup-saved");
  press(SEL);assert(app.screen==PACKAGES&&app.selected==1); // Reopening starts on the binding.
  press(M);
  memset(&app.auto_config,0,sizeof(app.auto_config));progAutoLoad();assert(!strcmp(app.auto_config.project[2],"esp_test"));
  /* Interrupted inactive-slot write keeps the last good binding on reload. */
  write_error=true;assert(!progAutoSave("other"));write_error=false;progAutoLoad();
  assert(!strcmp(app.auto_config.project[2],"esp_test"));
  /* SD removal clears bindings; inserting the same card restores them. */
  mounted=card=false;update(0,10);assert(!app.auto_config.project[2][0]);
  mounted=card=true;update(0,10);assert(!strcmp(app.auto_config.project[2],"esp_test"));
  press(M);press(M);assert(app.screen==HOME&&esp_jobs==1&&!probes);capture("home");
  press(SEL);assert(app.screen==RESULT&&app.auto_job&&!app.error[0]&&esp_jobs==2&&probes==1);
  capture("auto-done");update(SEL,2000);assert(esp_jobs==2); /* No repeat on hold. */
  press(SEL);assert(app.screen==HOME&&!app.auto_job&&esp_jobs==2); /* PASS: SEL returns home. */
  press(SEL);assert(esp_jobs==3&&probes==2); /* Next unit starts from home. */
  probe_status=SWD_ERR_MISMATCH;press(SEL);assert(app.screen==HOME);press(SEL);
  assert(probes==2&&esp_jobs==3&&strstr(app.error,"Unsupported SWD"));capture("auto-unsupported");
  probe_status=SWD_ERR_NORESP;probe_chip=ESP32C3_CHIP;press(SEL);
  assert(esp_jobs==3&&strstr(app.error,"Set Auto project"));capture("auto-unassigned");
  strcpy(app.auto_config.project[3],"esp_test");press(SEL);
  assert(esp_jobs==3&&strstr(app.error,"Auto chip mismatch"));
  probe_chip=ESP_UNKNOWN_CHIP;press(SEL);assert(esp_jobs==3&&strstr(app.error,"Unsupported ESP"));
  /* Retry with the card pulled fails before any probe; the failure stays on screen. */
  mounted=card=false;update(0,10);unsigned before=probes;press(SEL);
  assert(probes==before&&esp_jobs==3&&strstr(app.error,"SD not mounted"));
  capture("auto-failed");
  mounted=card=true;update(0,10);assert(app.screen==RESULT);
  /* H723 Auto uses SWD only and still passes the normal preflight. */
  strcpy(app.auto_config.project[0],"h723");probe_status=SWD_OK;fail_program=0;
  unsigned before_stm=probes;press(SEL);
  assert(programs==3&&probes==before_stm&&!app.error[0]&&app.result.started);
  capture("auto-done-h723");
  press(SEL);assert(app.screen==HOME&&!app.auto_job&&programs==3);capture("home-after-auto");
  /* Power: 3V3 and 5V switch independently and save to the card; Always keeps them on, Job only during a job. */
  assert(!app.power_on&&!gpio_out[TARGET_3V3]&&!gpio_out[TARGET_5V]&&!power_seen);
  press(M);while(app.menu_selected!=4)press(DN);
  press(SEL);assert(app.screen==POWER&&app.power_selected==0);capture("power");
  press(SEL);assert(app.power.out_3v3&&app.power_saved&&!gpio_out[TARGET_3V3]); /* Job: still off while idle. */
  press(DN);press(DN);press(SEL);assert(app.power.mode==PROG_POWER_ALWAYS&&gpio_out[TARGET_3V3]&&!gpio_out[TARGET_5V]);
  press(UP);press(SEL);assert(app.power.out_5v&&gpio_out[TARGET_3V3]&&gpio_out[TARGET_5V]);capture("power-always");
  press(M);press(M);assert(app.screen==HOME&&app.power_on);capture("home-power-both");
  press(M);press(SEL);assert(app.screen==POWER&&app.power_selected==1);
  press(SEL);assert(!app.power.out_5v&&!gpio_out[TARGET_5V]&&gpio_out[TARGET_3V3]);
  prog_power_config_t power_file;
  assert(progPowerLoad(&power_file)&&power_file.out_3v3&&!power_file.out_5v&&power_file.mode==PROG_POWER_ALWAYS);
  /* Job: Auto switches 3V_OUT on before the SWD probe, keeps it through the write and switches it off at the result. */
  press(DN);press(SEL);assert(app.power.mode==PROG_POWER_JOB&&!gpio_out[TARGET_3V3]);
  press(M);press(M);assert(app.screen==HOME&&!app.power_on);
  uint32_t power_start=ticks;press(SEL);
  assert(programs==4&&!app.error[0]&&power_seen==3&&!gpio_out[TARGET_3V3]&&ticks-power_start>=POWER_SETTLE_MS);
  press(SEL);assert(app.screen==HOME);
  /* A pulled card keeps the outputs; changes without a card are applied but not saved; a card without
   * power.cfg reads as all off. */
  app.power.mode=PROG_POWER_ALWAYS;progPowerChange();assert(gpio_out[TARGET_3V3]&&app.power_saved);
  mounted=card=false;update(0,10);assert(gpio_out[TARGET_3V3]&&app.power.out_3v3);
  press(M);press(SEL);assert(app.screen==POWER);press(UP);press(UP);press(SEL);
  assert(!app.power.out_3v3&&!gpio_out[TARGET_3V3]&&!app.power_saved);capture("power-not-saved");
  press(SEL);assert(gpio_out[TARGET_3V3]);press(M);while(app.menu_selected)press(UP);press(M);
  char power_path[512];assert(!remove(host("/prog/power.cfg",power_path)));
  mounted=card=true;update(0,10);
  assert(!app.power.out_3v3&&!app.power.out_5v&&app.power.mode==PROG_POWER_JOB&&app.power_saved&&!gpio_out[TARGET_3V3]);
  unlinks=0; /* power.cfg saves replace the old file; SD Delete below counts its own unlinks from zero. */
  mounted=card=false;update(0,10);before=probes;
  press(SEL);assert(app.screen==HOME&&probes==before); /* Start needs a mounted card. */
  mounted=card=true;update(0,10);assert(app.screen==HOME&&progAutoAssigned());capture("home-ready");
  /* Network -> Connect -> Wi-Fi: scan, type the password on the wheel, join, settings saved only after success. */
  press(M);press(DN);press(DN);press(DN);assert(app.screen==MENU && app.menu_selected==3);capture("menu-network");
  press(SEL);assert(app.screen==NETWORK);capture("network");
  press(SEL);assert(app.screen==CONNECT);capture("connect");
  press(SEL);assert(app.screen==WIFI_LIST && app.ap_count==3 && !app.ap_selected);capture("wifi-list");
  press(SEL);assert(app.screen==KEYBOARD && !strcmp(app.join_ssid,"Lab"));
  update(0,10);assert(!app.kbd.len); /* Releasing the SEL that opened the keyboard types nothing. */
  capture("keyboard");
  update(DN,10);update(DN,REPEAT_MS);update(DN,REPEAT_STEP_MS);update(0,10);assert(app.kbd.pick==3); /* Held DN repeats. */
  typeText("secrex");tap(M);typeText("t");assert(!strcmp(app.kbd.text,"secret"));capture("keyboard-typed");
  kbdMenu(0);assert(app.screen==RESULT && !app.error[0] && !strcmp(last_pw,"secret"));capture("wifi-joined");
  assert(settings.mode==NET_MODE_WIFI && !strcmp(settings.ssid,"Lab") && !strcmp(netStorePassword(&settings,"Lab"),"secret"));
  assert(settings_saves==1);
  press(SEL);assert(app.screen==NETWORK);
  /* Saved password is offered for editing; a wrong one fails and returns to the list, the saved one stays. */
  press(SEL);press(SEL);press(SEL);assert(app.screen==KEYBOARD && !strcmp(app.kbd.text,"secret"));
  update(0,10);tap(M);tap(M);kbdMenu(0);assert(app.screen==RESULT && strstr(app.error,"Wrong password"));capture("wifi-failed");
  assert(!strcmp(netStorePassword(&settings,"Lab"),"secret") && settings_saves==1);
  press(SEL);assert(app.screen==WIFI_LIST);
  press(DN);press(SEL);assert(app.screen==RESULT && !app.error[0] && !strcmp(settings.ssid,"Cafe")); /* Open network: no keyboard. */
  press(SEL);assert(app.screen==NETWORK);
  /* Keyboard: menu Clear, then M on the empty field leaves; the last row rescans. */
  press(SEL);press(SEL);press(SEL);assert(app.screen==KEYBOARD);update(0,10);
  kbdMenu(6);assert(app.screen==KEYBOARD && !app.kbd.len);capture("keyboard-menu");
  tap(M);assert(app.screen==WIFI_LIST);
  press(UP);assert(app.ap_selected==3);capture("wifi-rescan");press(SEL);assert(app.screen==WIFI_LIST && !app.ap_selected);
  /* LTE: APN preset, join, saved. */
  press(M);press(DN);press(SEL);assert(app.screen==APN_LIST);capture("apn-list");
  press(DN);press(SEL);assert(app.screen==RESULT && !app.error[0] && !strcmp(last_apn,"lte.ktfwing.com"));
  assert(settings.mode==NET_MODE_LTE && !strcmp(settings.apn,"lte.ktfwing.com"));
  press(SEL);assert(app.screen==NETWORK);
  press(DN);press(DN);press(SEL);assert(app.screen==NET_STATUS);capture("net-status");
  press(M);assert(app.screen==NETWORK);
  /* Install: Hub projects by chip; "=" marks the version already on the card; a hold installs. */
  press(UP);assert(app.network_selected==1);press(SEL);
  assert(app.screen==CHIPS && app.pick==PICK_HUB && progChipRows()==PROG_TARGETS && hub_fetches==1);capture("hub-chips");
  app.target=0;press(DN);press(DN);press(SEL);assert(app.screen==HUB_LIST && app.hub_view_count==2);capture("hub-list");
  row_t row={"","",NULL,-1};progHubRow(0,&row);assert(!strcmp(row.right,"="));
  row=(row_t){"","",NULL,-1};progHubRow(1,&row);assert(!row.right[0]);
  press(SEL);assert(app.screen==HUB_CONFIRM);capture("hub-confirm");
  update(SEL,2000);assert(!hub_installs); /* Opening press cannot confirm. */
  update(0,10);update(SEL,10);update(SEL,1001);assert(hub_installs==1 && !strcmp(last_install,"esp_test"));
  assert(app.screen==RESULT && !app.error[0]);capture("hub-done");
  press(SEL);assert(app.screen==HUB_LIST);
  /* STM32 projects need the ST loader, which is not on the Hub: Install is blocked with a message. */
  press(M);press(UP);press(UP);press(SEL);assert(app.screen==HUB_LIST && !strcmp(progHubAt(0)->id,"h7"));
  char loader[512],moved[520];host("/prog/loaders/st/0x483.stldr",loader);snprintf(moved,sizeof(moved),"%s.off",loader);
  assert(!rename(loader,moved));progRefresh();
  press(SEL);assert(app.screen==HUB_CONFIRM && !strcmp(progHubBlocked(progHubAt(0)),"No ST loader!"));capture("hub-no-loader");
  update(0,10);update(SEL,10);update(SEL,1500);assert(hub_installs==1);
  assert(!rename(moved,loader));progRefresh();assert(!progHubBlocked(progHubAt(0)));
  esp_info.state=NET_STATE_IDLE;assert(!strcmp(progHubBlocked(progHubAt(0)),"Not connected"));
  update(0,10);update(SEL,10);update(SEL,1500);assert(hub_installs==1);
  esp_info.state=NET_STATE_CONNECTED;hub_install_error="CRC mismatch";
  update(0,10);update(SEL,10);update(SEL,1001);assert(hub_installs==2 && app.screen==RESULT && strstr(app.error,"CRC"));
  hub_install_error=NULL;press(SEL);assert(app.screen==HUB_LIST);
  press(M);press(M);assert(app.screen==NETWORK);
  hub_list_error="HTTP 404";press(SEL);assert(app.screen==RESULT && strstr(app.error,"404"));
  press(SEL);assert(app.screen==NETWORK);hub_list_error=NULL;
  /* Off is saved too; without the ESP the page says so and Install fails clearly. */
  press(UP);press(SEL);assert(app.screen==CONNECT && app.connect_selected==1);press(DN);press(SEL);
  assert(app.screen==RESULT && !app.error[0] && settings.mode==NET_MODE_OFF && offs>=1);
  press(SEL);assert(app.screen==NETWORK);
  esp_present=false;app.net_poll=0;status_calls=0;
  for(unsigned i=0;i<20;i++)update(0,500); /* Network page open for 10 s without an ESP: no link calls from the UI. */
  assert(!status_calls && !strcmp(progNetState(),"No ESP"));capture("network-no-esp");
  { /* Labels: the module and the network are told apart. */
    net_info_t saved=app.net_info;esp_present=true;
    memset(&app.net_info,0,sizeof(app.net_info));assert(!strcmp(progNetState(),"ESP ready"));
    app.net_info.mode=NET_MODE_WIFI;assert(!strcmp(progNetState(),"ESP ready")); /* Scanned, not joined. */
    app.net_info.state=NET_STATE_CONNECTING;assert(!strcmp(progNetState(),"Wi-Fi..."));
    app.net_info.state=NET_STATE_CONNECTED;assert(!strcmp(progNetState(),"Wi-Fi on"));
    app.net_info.mode=NET_MODE_LTE;app.net_info.state=NET_STATE_FAILED;assert(!strcmp(progNetState(),"LTE fail"));
    app.net_info=saved;esp_present=false;
  }
  press(DN);press(SEL);assert(app.screen==RESULT && strstr(app.error,"No ESP link"));
  press(SEL);esp_present=true;
  press(M);press(UP);press(UP);press(UP);assert(app.screen==MENU && app.menu_selected==0);press(M);assert(app.screen==HOME);
  /* Storage -> SD Delete: count first, then delete children before folders; nothing is touched past the depth limit. */
  press(M);assert(app.menu_selected==0);press(DN);press(DN);press(SEL);assert(app.screen==STORAGE);
  press(DN);assert(app.storage_selected==1);capture("storage-delete");
  press(SEL);assert(app.screen==WIPE_CONFIRM && app.job==JOB_WIPE);capture("wipe-confirm");
  folder("/a");folder("/a/b");folder("/a/b/c");folder("/a/b/c/d");folder("/a/b/c/d/e");
  folder("/a/b/c/d/e/f");folder("/a/b/c/d/e/f/g");folder("/a/b/c/d/e/f/g/h");folder("/a/b/c/d/e/f/g/h/i");
  unsigned files=entries("/");
  confirmWrite();assert(app.screen==RESULT && strstr(app.error,"too deep") && !unlinks && entries("/")==files);
  char path[512];assert(!rmdir(host("/a/b/c/d/e/f/g/h/i",path)));
  press(M);assert(app.screen==STORAGE);press(SEL);
  mounted=card=false;update(SEL,2000);assert(app.screen==WIPE_CONFIRM);capture("wipe-no-card");
  update(0,10);update(SEL,1001);assert(!unlinks); // No card: holding does nothing.
  mounted=card=true;update(SEL,2000);assert(!unlinks);
  stop_at_unlink=3;update(0,10);update(SEL,10);update(SEL,1001);
  assert(app.screen==RESULT && strstr(app.error,"Stopped") && unlinks==3 && entries("/")>0);
  stop_at_unlink=-1;press(SEL);assert(app.screen==STORAGE);
  press(SEL);confirmWrite();
  assert(app.screen==RESULT && !app.error[0] && entries("/")==0);
  assert(!app.catalog.count && !progAutoAssigned() && !app.loader_present);capture("wipe-done");
  press(SEL);assert(app.screen==STORAGE);
  press(UP);press(SEL);press(SEL);assert(app.screen==PACKAGES && !app.view_count);capture("installed-empty");
  /* Past 16 packages the smallest IDs in strcmp order stay, whatever order the card lists them in. */
  folder("/prog");folder("/prog/fw");
  for(unsigned i=0;i<17;i++){
    char name[64];if(i<16)snprintf(name,sizeof(name),"/prog/fw/a%02u",i);else strcpy(name,"/prog/fw/Z0");
    folder(name);strcat(name,"/fw.txt");touch(name,"name=x\ndevice=RP2040\n");
  }
  mounted=card=false;update(0,10);mounted=card=true;update(0,10);
  assert(app.catalog.count==16 && app.catalog.more);
  bool z0=false,a15=false;
  for(unsigned i=0;i<app.catalog.count;i++){z0|=!strcmp(app.catalog.entries[i].id,"Z0");a15|=!strcmp(app.catalog.entries[i].id,"a15");}
  assert(z0 && !a15);
  press(M);assert(app.screen==CHIPS && app.pick==PICK_INSTALLED);capture("installed-chips-full");
  puts("PASS: chip table, catalog grouping by fw.txt device, usable/listed filters, ignored folders, sort order;");
  puts("PASS: release.txt chip/version rules (CRLF, comments, unknown keys, duplicate, schema, mismatch), 16+ by ID;");
  puts("PASS: Auto home start/setup/no-card gates, menu round trips (Auto setup / Manual / Storage / Info);");
  puts("PASS: Power: independent 3V3/5V, Always vs Job (on for probe+write, off at result), card save/reload, no-card.");
  puts("PASS: Auto filtering, save/readback, reopen on binding, interrupted-write recovery, card swap, detect/dispatch,");
  puts("      no held-key repeat, PASS returns home, unsupported SWD never tries ESP, unassigned/mismatched/SD-missing gates.");
  puts("PASS: Manual Flash/Erase via chip lists, ESP confirmation, dispatch, CRC gate, file closure and STM32 return;");
  puts("      release-before-hold confirmation, back, card removal, success/failure display.");
  puts("PASS: Network: Wi-Fi scan/list/keyboard (wheel, repeat, menu, clear, cancel), join saves only on success,");
  puts("      saved password offered, wrong password, open network, LTE APN, status, Off, no ESP;");
  puts("PASS: Hub install: chip groups, =/update marks, hold to install, ST loader/connection gates, failures;");
  puts("PASS: Installed bin by chip incl. unusable/unknown packages and details; SD Delete depth gate, no-card gate,");
  puts("      stop mid-way, read-only file, full wipe with Auto/catalog reset; 128x64 text bounds and snapshots");
}
