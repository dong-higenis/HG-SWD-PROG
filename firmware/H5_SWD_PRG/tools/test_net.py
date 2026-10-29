"""Host test for the H5 network side: ESP link framing/retries, settings record, Hub manifest reader, latest.txt and install."""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import zlib

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc', default=shutil.which('gcc'))
p.add_argument('--work', type=Path, default=root / 'build/host-tests/net')
p.add_argument('--manifest', type=Path, help='Optional real Hub manifest.json to read as well')
a = p.parse_args()
if not a.cc:
    p.error('Provide --cc with a native GCC compiler')
work = a.work
if work.exists():
    shutil.rmtree(work)
site, sd = work / 'site', work / 'sd'
sd.mkdir(parents=True)

# A small Hub: escapes, non-ASCII text, "id" keys inside nested objects and an unknown chip.
image = bytes((i * 31) & 255 for i in range(5000))
esp_bins = {'bootloader.bin': bytes(range(256)) * 3, 'firmware.bin': bytes(range(200)) * 9}
fw_txt = (b'name=Demo\ndevice=ESP32-S3\nbaud=auto\nflash_size=0x01000000\n'
          + b''.join(f'image={n},0x{addr:08X},0x{len(d):08X},0x{zlib.crc32(d):08X}\n'.encode()
                     for (n, d), addr in zip(esp_bins.items(), (0, 0x10000))))
release = b'schema=1\nid=demo-s3\nchip=esp32s3\nversion=1.2.0\ndate=2026-10-13\n'
files = {**esp_bins, 'release.txt': release, 'fw.txt': fw_txt}
manifest = {
    'schema': 1, 'generated': 'x', 'chips': [{'id': 'esp32s3', 'name': 'ESP32-S3'}],
    'loaders': [{'id': 'st-0x483', 'sd_path': '/prog/loaders/st/0x483.stldr'}],
    'projects': [
        {'id': 'demo-s3', 'name': 'Demo "quoted" \\ S3', 'chip': 'esp32s3', 'description': '한글 설명 é',
         'latest': '1.2.0', 'versions': [{'version': '1.2.0', 'images': [{'id': 'nested', 'name': 'x'}]}]},
        {'id': 'h7', 'name': 'H7 board', 'chip': 'stm32h723', 'latest': '2.0.0-rc.1', 'versions': []},
        {'id': 'future', 'name': 'Future chip', 'chip': 'stm32h730', 'latest': '0.1.0'},
        {'id': 'bad id!', 'name': 'Rejected', 'chip': 'esp32', 'latest': '1.0.0'},
    ],
    'build': {'id': 'not-a-project'},
}
(site / 'fw/demo-s3/1.2.0').mkdir(parents=True)
(site / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
lines = ['schema=1', 'id=demo-s3', 'chip=esp32s3', 'version=1.2.0', 'date=2026-10-13', 'base=fw/demo-s3/1.2.0/']
for name, data in files.items():
    (site / 'fw/demo-s3/1.2.0' / name).write_bytes(data)
    lines.append(f'file={name},{len(data)},0x{zlib.crc32(data):08X},{"0" * 64}')
(site / 'fw/demo-s3/latest.txt').write_text('\r\n'.join(lines) + '\r\nfuture=ignored\r\n', encoding='ascii')
# Card already holding an older demo-s3 with a stale fw.txt.
(sd / 'prog/fw/demo-s3').mkdir(parents=True)
(sd / 'prog/fw/demo-s3/fw.txt').write_bytes(b'name=Old\ndevice=ESP32-S3\n')
(sd / 'prog/fw/demo-s3/firmware.bin').write_bytes(b'old')

(work / 'hw_def.h').write_text('#pragma once\n#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n', encoding='ascii')
(work / 'fatfs.h').write_text(r'''
#pragma once
#include "hw_def.h"
#include <stdio.h>
typedef unsigned UINT;
typedef unsigned FRESULT;
typedef struct { FILE *fp; uint32_t size; } FIL;
typedef struct { void *handle; char path[512]; } DIR;
typedef struct { char fname[256]; unsigned fattrib; uint32_t fsize; } FILINFO;
enum { FR_OK=0, FR_INT_ERR=2, FR_NO_FILE=4, FR_NO_PATH=5, FR_DENIED=7, FR_EXIST=8 };
#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_ALWAYS 8
#define AM_RDO 1
#define AM_DIR 16
#define f_size(p) ((p)->size)
FRESULT f_open(FIL *,const char *,unsigned);
FRESULT f_read(FIL *,void *,UINT,UINT *);
FRESULT f_write(FIL *,const void *,UINT,UINT *);
FRESULT f_sync(FIL *);
FRESULT f_close(FIL *);
FRESULT f_stat(const char *,FILINFO *);
FRESULT f_opendir(DIR *,const char *);
FRESULT f_readdir(DIR *,FILINFO *);
FRESULT f_closedir(DIR *);
FRESULT f_unlink(const char *);
FRESULT f_mkdir(const char *);
FRESULT f_rename(const char *,const char *);
''', encoding='ascii')
(work / 'host_dir.c').write_text(r'''
#include <dirent.h>
#include <string.h>
void *hostOpenDir(const char *path) { return opendir(path); }
int hostCloseDir(void *dir) { return closedir(dir); }
const char *hostReadDir(void *dir) {
  struct dirent *e;
  while((e=readdir(dir)) && (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")));
  return e?e->d_name:NULL;
}
''', encoding='ascii')
source = work / 'test_net.c'
source.write_text(r'''
#include "net_link.h"
#include "net_store.h"
#include "net.h"
#include "hub.h"
#include "fatfs.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
void *hostOpenDir(const char *path);
int hostCloseDir(void *dir);
const char *hostReadDir(void *dir);
static const char *sd,*site;
static char *host(const char *name,char *out,const char *base) { snprintf(out,512,"%s%s",base,name);return out; }
/* ---- FatFs on the fixture card ---- */
FRESULT f_open(FIL *f,const char *name,unsigned mode) {
  char path[512];f->fp=fopen(host(name,path,sd),(mode&FA_WRITE)?"wb":"rb");if(!f->fp)return FR_NO_FILE;
  fseek(f->fp,0,SEEK_END);f->size=(uint32_t)ftell(f->fp);rewind(f->fp);return FR_OK;
}
FRESULT f_read(FIL *f,void *p,UINT n,UINT *got) { *got=(UINT)fread(p,1,n,f->fp);return FR_OK; }
FRESULT f_write(FIL *f,const void *p,UINT n,UINT *got) { *got=(UINT)fwrite(p,1,n,f->fp);return FR_OK; }
FRESULT f_sync(FIL *f) { return fflush(f->fp)?FR_INT_ERR:FR_OK; }
FRESULT f_close(FIL *f) { return fclose(f->fp)?FR_INT_ERR:FR_OK; }
FRESULT f_stat(const char *n,FILINFO *i) {
  char path[512];struct stat st;if(stat(host(n,path,sd),&st))return FR_NO_FILE;
  i->fsize=(uint32_t)st.st_size;i->fattrib=S_ISDIR(st.st_mode)?AM_DIR:0;return FR_OK;
}
FRESULT f_opendir(DIR *d,const char *n) { host(n,d->path,sd);d->handle=hostOpenDir(d->path);return d->handle?FR_OK:FR_NO_PATH; }
FRESULT f_readdir(DIR *d,FILINFO *i) {
  const char *name=hostReadDir(d->handle);i->fname[0]=0;if(!name)return FR_OK;
  char path[1024];snprintf(i->fname,sizeof(i->fname),"%s",name);snprintf(path,sizeof(path),"%s/%s",d->path,name);
  struct stat st;stat(path,&st);i->fattrib=S_ISDIR(st.st_mode)?AM_DIR:0;i->fsize=(uint32_t)st.st_size;return FR_OK;
}
FRESULT f_closedir(DIR *d) { return hostCloseDir(d->handle)?FR_INT_ERR:FR_OK; }
FRESULT f_unlink(const char *n) { char path[512];host(n,path,sd);struct stat st;if(stat(path,&st))return FR_NO_FILE;return (S_ISDIR(st.st_mode)?rmdir(path):remove(path))?FR_DENIED:FR_OK; }
FRESULT f_mkdir(const char *n) { char path[512];host(n,path,sd);struct stat st;if(!stat(path,&st))return FR_EXIST;return mkdir(path)?FR_NO_PATH:FR_OK; }
FRESULT f_rename(const char *a,const char *b) { char x[512],y[512];return rename(host(a,x,sd),host(b,y,sd))?FR_DENIED:FR_OK; }
/* ---- ESP HTTP simulated from the fixture site ---- */
static FILE *http;static int http_chunk=700,corrupt_at=-1,stop_after=-1;static unsigned http_gets,progress_calls;
static char last_url[300];
uint8_t netHttpGet(const char *url,uint16_t *code,uint32_t *length) {
  http_gets++;snprintf(last_url,sizeof(last_url),"%s",url);
  if(http){fclose(http);}
  http=NULL;*length=UINT32_MAX;
  assert(!strncmp(url,HUB_URL,strlen(HUB_URL)));
  char path[512];snprintf(path,sizeof(path),"%s/%s",site,url+strlen(HUB_URL));
  http=fopen(path,"rb");*code=http?200:404;
  if(http){fseek(http,0,SEEK_END);*length=(uint32_t)ftell(http);rewind(http);}
  return NET_ST_OK;
}
uint8_t netHttpRead(void *data,uint16_t max,uint16_t *size) {
  if(!max||max>NET_HTTP_READ_MAX){*size=0;return NET_ST_BAD_REQUEST;} /* As the ESP does. */
  assert(http);uint16_t n=max<http_chunk?max:(uint16_t)http_chunk;long at=ftell(http);
  *size=(uint16_t)fread(data,1,n,http);
  if(corrupt_at>=0 && strstr(last_url,"firmware.bin") && corrupt_at>=at && corrupt_at<at+*size)((uint8_t*)data)[corrupt_at-at]^=1;
  return NET_ST_OK;
}
uint8_t netHttpClose(void) { if(http)fclose(http);http=NULL;return NET_ST_OK; }
const char *netStatusText(uint8_t s) { return s?"net error":"OK"; }
static bool progress(void *c,const char *stage,uint32_t done,uint32_t total) {
  (void)c;assert(stage&&done<=total);progress_calls++;return stop_after<0||(int)progress_calls<stop_after;
}
/* ---- ESP side of the link, simulated over in-memory pipes ---- */
static uint8_t to_esp[4096],to_h5[4096];static unsigned esp_n,h5_in,h5_out;static uint32_t now;
static int drop_replies,garbage;static uint8_t last_seq;static unsigned esp_calls;
static net_frame_rx_t esp_rx;
static void espReply(const uint8_t *frame) {
  uint8_t cmd=frame[2],seq=frame[3];uint8_t payload[16]={0};uint16_t n=1;
  if(cmd==NET_CMD_HELLO){payload[1]=1;memcpy(payload+2,"T1",2);n=4;}
  else if(cmd==NET_CMD_STATUS){payload[1]=1;payload[2]=2;payload[3]=(uint8_t)-61;payload[4]=10;payload[7]=7;memcpy(payload+8,"lab",3);n=11;}
  else payload[0]=NET_ST_UNKNOWN_CMD;
  esp_calls++;last_seq=seq;
  if(drop_replies>0){drop_replies--;return;}
  if(garbage){to_h5[h5_in++]=0x5A;to_h5[h5_in++]=0xA5;to_h5[h5_in++]=0x00;}
  h5_in+=netLinkEncode(cmd|0x80,(uint8_t)(seq-1),payload,n,to_h5+h5_in); /* Stale seq: must be ignored. */
  h5_in+=netLinkEncode(cmd|0x80,seq,payload,n,to_h5+h5_in);
}
static uint32_t ioAvailable(void){return h5_in-h5_out;}
static uint8_t ioRead(void){return to_h5[h5_out++];}
static uint32_t ioWrite(const uint8_t *d,uint32_t n){
  h5_in=h5_out=0;
  for(uint32_t i=0;i<n;i++){to_esp[esp_n++]=d[i];if(netLinkRxFeed(&esp_rx,d[i]))espReply(esp_rx.frame);}
  return n;
}
static uint32_t ioMillis(void){return now;}
static void ioIdle(void){now++;}
static const net_link_io_t io={ioAvailable,ioRead,ioWrite,ioMillis,ioIdle};
static unsigned exists(const char *name){char path[512];struct stat st;return !stat(host(name,path,sd),&st);}
static long fsize(const char *name){char path[512];struct stat st;return stat(host(name,path,sd),&st)?-1:(long)st.st_size;}
int main(int argc,char **argv) {
  assert(argc>=3);sd=argv[1];site=argv[2];
  /* Link: CRC vector, round trip, resync on garbage, retries with the same seq, stale replies ignored. */
  assert(netLinkCrc((const uint8_t*)"123456789",9)==0x29B1);
  netLinkBind(&io);
  uint8_t data[64];uint16_t got;
  assert(netLinkCall(NET_CMD_HELLO,NULL,0,data,sizeof(data),&got,300,3)==NET_ST_OK && got==3 && data[0]==1 && !memcmp(data+1,"T1",2));
  garbage=1;assert(netLinkCall(NET_CMD_STATUS,NULL,0,data,sizeof(data),&got,300,3)==NET_ST_OK && got==10 && (int8_t)data[2]==-61);
  uint8_t seq=last_seq;drop_replies=2;esp_calls=0;
  assert(netLinkCall(NET_CMD_HELLO,NULL,0,data,sizeof(data),&got,300,3)==NET_ST_OK && esp_calls==3 && last_seq==(uint8_t)(seq+1));
  drop_replies=3;assert(netLinkCall(NET_CMD_HELLO,NULL,0,data,sizeof(data),&got,300,3)==NET_ST_NO_LINK);
  drop_replies=0;assert(netLinkCall(0x55,NULL,0,data,sizeof(data),&got,300,1)==NET_ST_UNKNOWN_CMD && got==0);
  uint8_t big[NET_LINK_PAYLOAD_MAX+1]={0};assert(netLinkCall(NET_CMD_HELLO,big,sizeof(big),NULL,0,NULL,300,1)==NET_ST_NO_LINK);
  /* Settings record: round trip, wrong device key or one flipped bit is rejected, most recent network first. */
  uint8_t key[12]={1,2,3,4,5,6,7,8,9,10,11,12};netStoreSetKey(key);
  net_store_t s={0},t;s.mode=NET_MODE_WIFI;strcpy(s.apn,"mobile");strcpy(s.ssid,"lab");
  for(unsigned i=0;i<10;i++){char n[16],w[16];snprintf(n,16,"ap%u",i);snprintf(w,16,"pw%u",i);netStoreRemember(&s,n,w);}
  assert(s.count==NET_STORE_NETWORKS && !strcmp(s.known[0].ssid,"ap9") && !strcmp(s.known[7].ssid,"ap2"));
  netStoreRemember(&s,"ap5","new");assert(s.count==8 && !strcmp(s.known[0].ssid,"ap5") && !strcmp(netStorePassword(&s,"ap5"),"new"));
  assert(!netStorePassword(&s,"ap0") && !strcmp(s.known[1].ssid,"ap9"));
  static uint8_t record[NET_STORE_SIZE];netStoreEncode(&s,record);
  for(unsigned i=0;i+3<=sizeof(record);i++)assert(memcmp(record+i,"pw9",3)); /* Not readable as plain text. */
  assert(netStoreDecode(record,&t) && !memcmp(&s,&t,sizeof(s)));
  for(unsigned i=0;i<sizeof(record);i+=37){record[i]^=4;assert(!netStoreDecode(record,&t));record[i]^=4;}
  uint8_t other[12]={9};netStoreSetKey(other);assert(!netStoreDecode(record,&t)&&!t.count); /* Another programmer reads it as empty. */
  netStoreSetKey(key);assert(netStoreSave(&s) && exists("/net/net.cfg") && !exists("/net/net.tmp"));
  assert(netStoreLoad(&t) && !strcmp(t.ssid,"lab") && !strcmp(netStorePassword(&t,"ap5"),"new"));
  /* Manifest: projects only, escapes kept, nested "id" ignored, bad IDs skipped, chips mapped. */
  static hub_list_t list,again;static hub_json_t json;
  char path[512];snprintf(path,sizeof(path),"%s/manifest.json",site);FILE *f=fopen(path,"rb");assert(f);
  static uint8_t text[65536];size_t n=fread(text,1,sizeof(text),f);fclose(f);
  hubJsonBegin(&json,&list);hubJsonFeed(&json,text,(uint32_t)n);assert(!json.error && !json.depth);
  hubJsonBegin(&json,&again);for(size_t i=0;i<n;i++)hubJsonFeed(&json,text+i,1);
  assert(!memcmp(&list,&again,sizeof(list)));
  assert(list.count==3 && !list.more);
  assert(!strcmp(list.projects[0].id,"demo-s3") && !strcmp(list.projects[0].name,"Demo \"quoted\" \\ S3"));
  assert(!strcmp(list.projects[0].version,"1.2.0") && list.projects[0].target==PROG_TARGET_ESP32S3);
  assert(!strcmp(list.projects[1].version,"2.0.0-rc.1") && list.projects[1].target==PROG_TARGET_H723);
  assert(!strcmp(list.projects[2].id,"future") && list.projects[2].target==PROG_TARGET_NONE);
  hubJsonBegin(&json,&again);hubJsonFeed(&json,(const uint8_t*)"{\"projects\":[{\"id\":\"x\"}",22);assert(json.depth); /* Truncated body. */
  hubJsonBegin(&json,&again);
  for(unsigned i=0;i<40;i++){char one[64];int k=snprintf(one,64,"%s{\"id\":\"p%02u\",\"chip\":\"esp32\"}",i?",":"{\"projects\":[",i);hubJsonFeed(&json,(uint8_t*)one,(uint32_t)k);}
  hubJsonFeed(&json,(const uint8_t*)"]}",2);assert(again.count==HUB_PROJECTS && again.more && !json.depth);
  if(argc>3){ /* The live manifest, when given. */
    FILE *r=fopen(argv[3],"rb");assert(r);n=fread(text,1,sizeof(text),r);fclose(r);
    hubJsonBegin(&json,&again);hubJsonFeed(&json,text,(uint32_t)n);assert(!json.error && !json.depth && again.count);
    for(unsigned i=0;i<again.count;i++)printf("  hub: %-24s %-10s %-10s %s\n",again.projects[i].id,again.projects[i].chip,again.projects[i].version,again.projects[i].name);
  }
  /* latest.txt rules. */
  hub_latest_t latest;char bad[256];
  strcpy(bad,"schema=2\nid=a\nbase=fw/a/1/\nfile=fw.txt,1,0x0,x\n");assert(!strcmp(hubLatestParse(bad,&latest),"latest.txt schema"));
  strcpy(bad,"schema=1\nid=a\nbase=fw/../x/\nfile=fw.txt,1,0x0,x\n");assert(!strcmp(hubLatestParse(bad,&latest),"latest.txt base"));
  strcpy(bad,"schema=1\nid=a\nbase=fw/a/1/\nfile=../fw.txt,1,0x0,x\n");assert(!strcmp(hubLatestParse(bad,&latest),"latest.txt name"));
  strcpy(bad,"schema=1\nid=a\nbase=fw/a/1/\nfile=fw.txt,1,0x0,x\nfile=a.bin,2,0x1,y\n");assert(!strcmp(hubLatestParse(bad,&latest),"latest.txt order"));
  strcpy(bad,"schema=1\nid=a\nbase=fw/a/1/\nfile=fw.txt,1x,0x0,x\n");assert(!strcmp(hubLatestParse(bad,&latest),"latest.txt number"));
  /* Install: a CRC failure leaves no fw.txt (package hidden), a stop too; a clean run writes every file. */
  corrupt_at=1000;http_gets=0;
  const char *e=hubInstall(&list.projects[0],progress,NULL);assert(e && !strcmp(e,"CRC mismatch"));
  assert(!exists("/prog/fw/demo-s3/fw.txt") && !exists("/prog/fw/demo-s3/firmware.bin.part") && exists("/prog/fw/demo-s3/bootloader.bin"));
  corrupt_at=-1;progress_calls=0;stop_after=3;e=hubInstall(&list.projects[0],progress,NULL);assert(e && !strcmp(e,"Stopped") && !exists("/prog/fw/demo-s3/fw.txt"));
  stop_after=-1;http_chunk=333;e=hubInstall(&list.projects[0],progress,NULL);assert(!e);
  assert(fsize("/prog/fw/demo-s3/fw.txt")==@FW@ && fsize("/prog/fw/demo-s3/firmware.bin")==@BIN@ && fsize("/prog/fw/demo-s3/release.txt")==@REL@);
  assert(!exists("/prog/fw/demo-s3/fw.txt.part"));
  /* The SD catalog now sees the package with its Hub version. */
  static prog_catalog_t cat;assert(progCatalogScan(&cat)==FR_OK && cat.count==1 && !cat.entries[0].error);
  assert(cat.entries[0].target==PROG_TARGET_ESP32S3 && !strcmp(cat.entries[0].version,"1.2.0"));
  e=hubInstall(&list.projects[1],progress,NULL);assert(e && !strcmp(e,"HTTP 404")); /* Not on the site. */
  puts("PASS: link CRC/round trip/resync/retry same seq/stale seq/no link; settings record scramble, CRC, key, order, save/load;");
  puts("PASS: manifest stream (whole vs byte-wise, escapes, nested ids, bad ids, limit, truncation); latest.txt rules;");
  puts("PASS: install CRC failure/stop keep the package hidden, clean install visible in the catalog, HTTP 404");
  return 0;
}
'''.replace('@FW@', str(len(fw_txt))).replace('@BIN@', str(len(esp_bins['firmware.bin']))).replace('@REL@', str(len(release))), encoding='utf-8')
exe = work / 'test_net.exe'
mods = root / 'src/ap/modules'
subprocess.run([a.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(work), '-I' + str(mods / 'net'),
                '-I' + str(mods / 'prog'), '-I' + str(mods), str(source), str(work / 'host_dir.c'),
                *[str(mods / 'net' / f) for f in ['net_link.c', 'net_store.c', 'hub.c']],
                *[str(mods / 'prog' / f) for f in ['prog_catalog.c', 'prog_target.c', 'prog_inspect.c', 'esp_project.c']],
                '-o', str(exe)], check=True)
args = [str(exe), str(sd), str(site)] + ([str(a.manifest)] if a.manifest else [])
subprocess.run(args, check=True)
