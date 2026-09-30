
#include "swd_dap.h"
#include "swd_transport.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t memory[2048], tar, csw, posted, transfers, tar_writes, status_reads, flushes;
static unsigned mode, waits;
uint32_t millis(void) { return transfers; }
void swdPortBegin(uint32_t khz) { (void)khz; }
void swdRelease(void) {}
void swdWireConnect(void) {}
uint32_t swdWireTransfer(bool ap,bool read,uint8_t addr,uint32_t *v) {
  transfers++;
  if(mode==1 && transfers==7) { waits++; return SWD_ACK_WAIT; }
  if(mode==2)return SWD_ACK_WAIT;
  if(mode==3 && transfers==15)return SWD_ACK_FAULT;
  if(!ap) {
    if(read && addr==12) { *v=posted; flushes++; }
    else if(read && addr==4) { *v=mode==4?0x20:0;status_reads++; }
    else { assert(!read && addr==8 && *v==0); }
    return SWD_ACK_OK;
  }
  if(!read && addr==0)csw=*v;
  else if(!read && addr==4) { tar=*v;tar_writes++; }
  else {
    assert(addr==12 && csw==0x23000012);
    assert(tar>=0x20000000 && tar<0x20002000);
    unsigned index=(tar-0x20000000)/4;
    if(read) { uint32_t old=posted;posted=memory[index];*v=old; }
    else memory[index]=*v;
    tar=(tar&~1023U)|((tar+4)&1023); // Hardware wraps at 1KiB.
  }
  return SWD_ACK_OK;
}
static void setup(unsigned m) {
  mode=m;tar=csw=posted=transfers=tar_writes=status_reads=flushes=waits=0;
  for(unsigned i=0;i<2048;i++)memory[i]=0x3948a721U^(i*7919);
}
int main(void) {
  swd_probe_t p={0};uint32_t buf[1000], expected[1000];
  for(unsigned m=0;m<2;m++) {
    setup(m);memcpy(expected,memory+252,sizeof(expected));
    assert(swdMemReadWords(&p,0x200003f0,buf,1000)==SWD_OK);
    assert(!memcmp(buf,expected,sizeof(buf)) && tar_writes==5 && status_reads==5 && flushes==5);
    assert(waits==m);
    setup(m);for(unsigned i=0;i<1000;i++)buf[i]=~i;
    assert(swdMemWriteWords(&p,0x200003f0,buf,1000)==SWD_OK);
    assert(!memcmp(memory+252,buf,sizeof(buf)) && tar_writes==5 && status_reads==5 && flushes==5);
  }
  setup(2);assert(swdMemReadWords(&p,0x20000000,buf,10)==SWD_ERR_WAIT && transfers==64);
  setup(3);assert(swdMemWriteWords(&p,0x20000000,buf,100)==SWD_ERR_FAULT);
  setup(4);assert(swdMemReadWords(&p,0x20000000,buf,100)==SWD_ERR_FAULT);
  setup(0);assert(swdMemReadWords(&p,0x20000001,buf,1)==SWD_ERR_ARGUMENT);
  assert(swdMemWriteWords(&p,0xfffffff0,buf,100)==SWD_ERR_ARGUMENT && !transfers);
  puts("SWD burst: posted pipeline, 1KiB wrap boundaries, WAIT retry, sticky errors and bounds passed");
}
