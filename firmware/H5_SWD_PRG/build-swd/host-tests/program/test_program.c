
#include "swd_program.h"
#include "swd_dap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define BASE 0x08000000U
#define RAM 0x24000000U
#define HALT (1U<<17)
#define READY (1U<<16)
#define RESET (1U<<25)
static uint8_t ram[0x20000], flash[0x100000], image[0x21013];
static uint32_t code[505], regs[21], dh, dfsr, dcrdr, sr, time_ms, dbg3, dbg4, flash_cr;
static int scenario, erases, writes, starts, resets, release_count, init_calls, running_polls;
static bool cancelled, verify_phase, erase_mode;
static uint32_t clock_khz;
enum { GOOD, WRONG_ID, WRONG_SIZE, RDP, WRP, NO_RESET, CACHE, BAD_CODE, BAD_R0,
       BAD_STOP, EXCEPTION, RESET_WRITE, SR_ERROR, BAD_VERIFY, SD_FAIL, BAD_CRC,
       CANCEL_BEFORE, CANCEL_ERASE, TIMEOUT_WRITE, IO_ERROR, HALT_FAIL, READBACK_FAIL,
       FAST_FALLBACK, SLOW_COUNTER, TUNE_RESET, ERASE_IO_FAULT, WRP_LAST, BLANK_DIRTY, ERASE_SR_ERROR, ERASE_TIMEOUT, ERASE_CANCEL_VERIFY, LOCK_FAIL };
uint32_t millis(void) { return time_ms++; }
void swdPortBegin(uint32_t khz) { assert(khz==100); }
void swdPortSetSpeed(uint32_t khz) { assert(khz>=100 && khz<=12000); clock_khz=khz; }
uint32_t swdPortMaxKhz(void) { return scenario==SLOW_COUNTER?100:12000; }
void swdRelease(void) { release_count++; }
swd_err_t swdH723Connect(swd_probe_t *p) { p->last_ack=1; p->ctrl_stat=0xF0000000; return scenario==WRONG_ID?SWD_ERR_MISMATCH:SWD_OK; }
swd_err_t swdDapClearErrors(swd_probe_t *p) { (void)p; return SWD_OK; }
static uint32_t get(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static void put(uint8_t *p,uint32_t v) { memcpy(p,&v,4); }
static void execute(void) {
  uint32_t pc=regs[15];
  assert(regs[17]==0x24008000 && regs[20]==1 && regs[14]==0x24000001);
  assert(regs[16]==0x1000000);
  assert(!(dh&HALT));
  if(pc==0x2400000A) init_calls++;
  else if(pc==0x24000398) {
    assert(init_calls==1);
    assert(regs[0]>=BASE && regs[1]<BASE+sizeof(flash));
    assert((regs[0]&0x1ffff)==0 && regs[1]-regs[0]==0x1ffff);
    memset(flash+regs[0]-BASE,0xff,0x20000); erases++;
    if(scenario==BLANK_DIRTY && erases==8)flash[0xFFFFF]=0;
    if(scenario==ERASE_SR_ERROR)sr=0x40000;
    if(scenario==ERASE_TIMEOUT) { running_polls=1; return; }
  } else if(pc==0x24000040) {
    assert(!erase_mode);
    assert(erases==2 && (regs[0]&31)==0 && (regs[1]&31)==0 && regs[1]<=4096);
    assert(regs[2]==0x24002000);
    for(uint32_t i=0;i<regs[1];i++)assert(flash[regs[0]-BASE+i]==0xff);
    memcpy(flash+regs[0]-BASE,ram+regs[2]-RAM,regs[1]); writes++;
    if(scenario==TIMEOUT_WRITE || scenario==HALT_FAIL) { running_polls=1; return; }
    if(scenario==RESET_WRITE)dh|=RESET;
    if(scenario==SR_ERROR)sr=0x40000;
  } else assert(!"unexpected loader function");
  regs[0]=scenario==BAD_R0?0:1;
  regs[15]=scenario==BAD_STOP?0x24000200:RAM;
  if(scenario==EXCEPTION)regs[16]|=3;
  dfsr=2; dh|=HALT;
}
swd_err_t swdMemRead32(swd_probe_t *p,uint32_t addr,uint32_t *v) {
  (void)p;
  if(addr>=RAM && addr<RAM+sizeof(ram)) { *v=get(ram+addr-RAM); return SWD_OK; }
  if(addr>=BASE && addr<BASE+sizeof(flash)) { *v=get(flash+addr-BASE); return SWD_OK; }
  switch(addr) {
  case 0x1FF1E880: *v=scenario==WRONG_SIZE?512:1024; break;
  case 0x5200201C: *v=scenario==RDP?0:0xaa00; break;
  case 0x52002038: *v=scenario==WRP?0xfe:scenario==WRP_LAST?0x7f:0xff; break;
  case 0x5C001034: *v=dbg3; break;
  case 0x5C001054: *v=dbg4; break;
  case 0xE000EDF0: *v=dh; dh&=~RESET; break;
  case 0xE000EDF8: *v=dcrdr; break;
  case 0xE000ED30: *v=dfsr; break;
  case 0xE000ED14: *v=scenario==CACHE?(1U<<16):0; break;
  case 0xE000ED94: *v=0; break;
  case 0x52002010: *v=sr; break;
  case 0x5200200C: *v=flash_cr; break;
  default: fprintf(stderr,"unexpected read %08x\n",addr); abort();
  }
  return SWD_OK;
}
swd_err_t swdMemWrite32(swd_probe_t *p,uint32_t addr,uint32_t v) {
  (void)p;
  if(addr>=RAM && addr<RAM+sizeof(ram)) { put(ram+addr-RAM,v); return SWD_OK; }
  switch(addr) {
  case 0x5C001034: dbg3=v; break;
  case 0x5C001054: dbg4=v; break;
  case 0xE000EDFC: break;
  case 0xE000ED30: dfsr&=~v; break;
  case 0xE000EDF8: dcrdr=v; break;
  case 0xE000EDF4:
    assert(dh&HALT); assert((v&31)<21);
    if(v&0x10000)regs[v&31]=dcrdr; else dcrdr=regs[v&31];
    break;
  case 0x5200200C: if(scenario!=LOCK_FAIL)flash_cr=v; break;
  case 0x52002014:
    if(scenario==ERASE_IO_FAULT && erases==1) {
      p->last_ack=4; p->ctrl_stat=0xF0000020; return SWD_ERR_FAULT;
    }
    sr&=~v; break;
  case 0xE000ED0C:
    assert(v==0x05fa0004); resets++;
    if(scenario==NO_RESET)break;
    dh=READY|HALT|RESET|3; dfsr=8; memset(regs,0,sizeof(regs)); regs[16]=0x1000000;
    regs[15]=get(flash+4)&~1U;
    sr=0; break;
  case 0xE000EDF0: {
    assert((v&0xffff0000)==0xa05f0000);
    if((dh^v)&8)assert((dh&HALT)&&(v&2));
    if(scenario==HALT_FAIL && running_polls)return SWD_ERR_NORESP;
    dh=(dh&(RESET|HALT))|READY|(v&15);
    if(v&2)dh|=HALT;
    else {
      dh&=~HALT;
      if(v&1)execute();
      else { assert(resets==2); starts++; }
    }
    break;
  }
  default: fprintf(stderr,"unexpected write %08x\n",addr); abort();
  }
  return SWD_OK;
}
swd_err_t swdMemWriteWords(swd_probe_t *p,uint32_t addr,const uint32_t *v,uint32_t n) {
  (void)p;
  assert(dh&HALT); assert(addr>=RAM && addr+4*n<=RAM+sizeof(ram));
  if(scenario==IO_ERROR && addr==0x24002000)return SWD_ERR_PARITY;
  memcpy(ram+addr-RAM,v,n*4); return SWD_OK;
}
swd_err_t swdMemReadWords(swd_probe_t *p,uint32_t addr,uint32_t *v,uint32_t n) {
  (void)p; assert(dh&HALT);
  if(addr>=RAM && addr+4*n<=RAM+sizeof(ram)) {
    memcpy(v,ram+addr-RAM,n*4);
    if((scenario==BAD_CODE && addr==RAM+4)||(scenario==READBACK_FAIL && addr==0x24002000))v[0]^=1;
    if(scenario==FAST_FALLBACK && clock_khz>2000 && addr==0x24002000)v[0]^=1;
    if(scenario==TUNE_RESET && addr==0x24002000)dh|=RESET;
  } else {
    assert(verify_phase && addr>=BASE && addr+4*n<=BASE+sizeof(flash));
    memcpy(v,flash+addr-BASE,n*4);
    if(scenario==BAD_VERIFY)v[0]^=1;
  }
  return SWD_OK;
}
static bool readImage(void *ctx,uint32_t off,void *buf,uint32_t n) {
  (void)ctx; assert(!erase_mode); assert(off+n<=sizeof(image));
  if(scenario==SD_FAIL)return false;
  memcpy(buf,image+off,n); return true;
}
static bool progress(void *ctx,const char *s,uint32_t done,uint32_t total) {
  (void)ctx; assert(done<=total);
  if(!strcmp(s,"VERIFY") || !strcmp(s,"BLANK CHECK"))verify_phase=true;
  if(scenario==ERASE_CANCEL_VERIFY && !strcmp(s,"BLANK CHECK"))cancelled=true;
  if(scenario==CANCEL_BEFORE && !strcmp(s,"INIT ALGORITHM"))cancelled=true;
  if(scenario==CANCEL_ERASE && erases==1)cancelled=true;
  return !cancelled;
}
static uint32_t crc32(const uint8_t *p,size_t n) {
  uint32_t v=~0U;
  while(n--) { v^=*p++; for(int i=0;i<8;i++)v=(v>>1)^(0xedb88320U&(0U-(v&1))); }
  return ~v;
}
int main(int argc,char **argv) {
  assert(argc==2);
  FILE *f=fopen(argv[1],"rb"); assert(f); assert(fseek(f,252,SEEK_SET)==0);
  assert(fread(code,1,sizeof(code),f)==sizeof(code)); fclose(f);
  for(unsigned i=0;i<sizeof(image);i++)image[i]=(uint8_t)(i*31+i/7);
  put(image,0x24050000); put(image+4,0x08000101);
  for(scenario=GOOD;scenario<=ERASE_IO_FAULT;scenario++) {
    memset(ram,0x37,sizeof(ram)); memset(flash,0xa5,sizeof(flash)); memset(regs,0,sizeof(regs));
    dh=READY|1; dfsr=sr=time_ms=0; dbg3=0x100;dbg4=0x200;
    erases=writes=starts=resets=release_count=init_calls=running_polls=0;
    cancelled=verify_phase=false;
    clock_khz=100;
    swd_program_input_t in={code,sizeof(code),sizeof(image),crc32(image,sizeof(image)),NULL,readImage,progress};
    if(scenario==BAD_CRC)in.image_crc^=1;
    swd_program_result_t r;
    swd_err_t e=swdProgramH723(&in,&r);
    assert(release_count==1);
    if(scenario==GOOD || scenario==FAST_FALLBACK || scenario==SLOW_COUNTER) {
      assert(e==SWD_OK && r.started && starts==1 && resets==2);
      assert(r.written==sizeof(image) && r.verified==sizeof(image));
      assert(!memcmp(flash,image,sizeof(image)));
      for(unsigned i=sizeof(image);i<0x40000;i++)assert(flash[i]==0xff);
      for(unsigned i=0x40000;i<sizeof(flash);i++)assert(flash[i]==0xa5);
      assert(dbg3==0x100 && dbg4==0x200);
      assert(r.clock_khz==(scenario==FAST_FALLBACK?2000U:scenario==SLOW_COUNTER?100U:12000U));
      assert(r.write_ms && r.verify_ms);
    } else {
      if(e==SWD_OK) { fprintf(stderr,"scenario %d unexpectedly succeeded\n",scenario); abort(); }
      assert(!r.started && starts==0);
      if(r.reset_attempted && scenario!=HALT_FAIL)assert(r.halted_on_error);
      if(scenario==CANCEL_BEFORE)assert(erases==0 && r.cancelled);
      if(scenario==CANCEL_ERASE)assert(erases==1 && writes==0 && r.cancelled);
      if(scenario==BAD_CODE || scenario==CACHE || scenario==NO_RESET || scenario<=WRP)assert(erases==0);
      if(scenario==HALT_FAIL)assert(!r.halted_on_error);
      if(scenario==SR_ERROR) {
        assert(r.flash_sr_valid && r.flash_status_error && r.flash_sr==0x40000);
        assert(!r.failed_io);
      }
      if(scenario==ERASE_IO_FAULT) {
        assert(e==SWD_ERR_FAULT && erases==1 && writes==0);
        assert(!r.flash_sr_valid && !r.flash_status_error);
        assert(r.failed_io && !strcmp(r.failed_io,"WRITE32"));
        assert(r.failed_io_address==0x52002014);
        assert(r.failed_ack==4 && r.failed_ctrl==0xF0000020);
        assert(r.link.last_ack==1 && r.link.ctrl_stat==0xF0000000);
      }
      if(scenario==TUNE_RESET || scenario==READBACK_FAIL)assert(!erases && !writes);
      if(r.reset_attempted)assert(clock_khz==100); // Recovery uses conservative timing.
    }
  }
  const int erase_cases[]={GOOD,WRONG_ID,WRONG_SIZE,RDP,WRP_LAST,NO_RESET,CACHE,BAD_CODE,
    BAD_R0,BAD_STOP,EXCEPTION,CANCEL_BEFORE,CANCEL_ERASE,ERASE_IO_FAULT,BLANK_DIRTY,
    ERASE_SR_ERROR,ERASE_TIMEOUT,ERASE_CANCEL_VERIFY,LOCK_FAIL};
  erase_mode=true;
  for(unsigned k=0;k<sizeof(erase_cases)/sizeof(erase_cases[0]);k++) {
    scenario=erase_cases[k];
    memset(ram,0x37,sizeof(ram));memset(flash,0xa5,sizeof(flash));memset(regs,0,sizeof(regs));
    dh=READY|1;dfsr=sr=time_ms=flash_cr=0;dbg3=0x100;dbg4=0x200;
    erases=writes=starts=resets=release_count=init_calls=running_polls=0;
    cancelled=verify_phase=false;clock_khz=100;
    swd_program_input_t in={code,sizeof(code),0,0,NULL,NULL,progress};
    swd_program_result_t r;swd_err_t e=swdEraseH723(&in,&r);
    assert(!writes && !starts && !r.started && !r.written && release_count==1);
    if(scenario==GOOD) {
      assert(e==SWD_OK && r.erase_complete && erases==8 && resets==1);
      assert(r.verified==sizeof(flash) && (dh&HALT) && (flash_cr&1));
      assert(r.erase_ms && r.verify_ms);
      for(unsigned i=0;i<sizeof(flash);i++)assert(flash[i]==0xFF);
    } else {
      assert(e!=SWD_OK && !r.erase_complete);
      if(r.reset_attempted)assert(r.halted_on_error);
      if(scenario==WRP_LAST || scenario==WRONG_ID || scenario==WRONG_SIZE || scenario==RDP)assert(erases==0);
      if(scenario==BLANK_DIRTY)assert(r.address==BASE+0xFFFFC && e==SWD_ERR_VERIFY);
      if(scenario==CANCEL_ERASE || scenario==ERASE_IO_FAULT)assert(erases==1);
      if(scenario==ERASE_SR_ERROR)assert(r.flash_status_error);
      if(scenario==ERASE_TIMEOUT)assert(e==SWD_ERR_TIMEOUT && erases==1);
      if(scenario==ERASE_CANCEL_VERIFY)assert(r.cancelled && r.verified==0);
    }
  }
  puts("PASS: erase-only all 8 sectors, full blank check, no image/read/write/resume,");
  puts("      last-sector protection, blank mismatch, flash errors, timeout, cancel and lock failure");
  puts("PASS: program/verify/reset, sector limits, 32-byte padding, protection, reset/cache gates,");
  puts("      I/O/corruption/loader failures, timeout, cancellation, failure halt and no automatic restart");
  puts("      fast link training, automatic fallback, stopped DWT fallback, reset during training, phase timers");
  return 0;
}
