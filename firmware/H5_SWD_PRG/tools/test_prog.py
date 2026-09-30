"""Compile the production file inspector and check real SD inputs plus malformed files."""
from pathlib import Path
import argparse
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc', default=shutil.which('gcc'))
p.add_argument('--sd', required=True, type=Path, help='Prepared SD root (or a directory with the same layout)')
a = p.parse_args()
if not a.cc:
    p.error('Provide --cc with a native GCC compiler')
work = root / 'build-swd/host-tests/prog'
work.mkdir(parents=True, exist_ok=True)
source = work / 'test_prog.c'
source.write_text(r'''
#include "prog_inspect.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char *data; uint32_t size; int calls, fail; } memory_t;
static bool rd(void *ctx, uint32_t off, void *buf, uint32_t n) {
  memory_t *m = ctx;
  assert(off <= m->size && n <= m->size-off);
  if (++m->calls == m->fail) return false;
  memcpy(buf, m->data+off, n); return true;
}
static memory_t load(const char *name) {
  FILE *f = fopen(name, "rb"); assert(f);
  assert(fseek(f, 0, SEEK_END) == 0);
  long n = ftell(f); assert(n >= 0 && n < 0x200000);
  rewind(f);
  memory_t m = {calloc((size_t)n+1, 1), (uint32_t)n, 0, -1}; assert(m.data);
  assert(fread(m.data, 1, (size_t)n, f) == (size_t)n); fclose(f); return m;
}
static uint32_t get32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void put32(unsigned char *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(unsigned char)(v>>(8*i)); }
static void badword(memory_t *m, prog_file_t *f, uint32_t offset, uint32_t value) {
  prog_loader_t out;
  uint32_t old = get32(m->data+offset);
  put32(m->data+offset,value);
  assert(progInspectLoader(f,&out));
  put32(m->data+offset,old);
}
int main(int argc, char **argv) {
  assert(argc == 4);
  memory_t lm=load(argv[1]), im=load(argv[2]), mm=load(argv[3]);
  prog_file_t lf={&lm,lm.size,rd}, inf={&im,im.size,rd};
  prog_manifest_t man;
  char config[1025]; assert(mm.size < sizeof(config));
  memcpy(config, mm.data, mm.size+1);
  assert(!progParseManifest(config,&man));
  assert(man.address==0x08000000 && !strcmp(man.image,"app.bin"));
  prog_loader_t l;
  const char *err=progInspectLoader(&lf,&l);
  if (err) { fprintf(stderr,"Real loader rejected: %s\n",err); return 1; }
  int reads=lm.calls;
  assert(l.init==0x2400000B && l.write==0x24000041 && l.erase==0x24000399);
  assert(l.mass_erase==0x2400034D && l.flash_size==0x100000 && l.sectors==8);
  assert(l.code_start==0x24000004 && l.code_end==0x240007E8 && l.page_hint==4096);
  for (int i=1;i<=reads;i++) {
    lm.calls=0; lm.fail=i; assert(progInspectLoader(&lf,&l));
  }
  lm.fail=-1;
  for (uint32_t n=0;n<lm.size;n+=31) {
    lf.size=n; assert(progInspectLoader(&lf,&l));
  }
  lf.size=lm.size;
  badword(&lm,&lf,0,0); // magic
  badword(&lm,&lf,16,0x00280003); // ET_DYN
  badword(&lm,&lf,28,0xfffffff0); // program table overflow
  badword(&lm,&lf,32,0xfffffff0); // section table overflow
  uint32_t ph=get32(lm.data+28), sh=get32(lm.data+32);
  badword(&lm,&lf,ph+4,0xfffffff0); // metadata file range
  badword(&lm,&lf,ph+32+8,0x08000000); // RAM segment address
  badword(&lm,&lf,ph+32+20,0xffffffff); // RAM size overflow
  badword(&lm,&lf,ph+32+24,4); // non-executable code
  uint32_t meta=get32(lm.data+ph+4);
  badword(&lm,&lf,meta+104,0x09000000);
  badword(&lm,&lf,meta+108,0x200000);
  badword(&lm,&lf,meta+120,9);
  badword(&lm,&lf,meta+128,1);
  uint32_t syms=0, names=0, nsyms=0;
  unsigned nsh=lm.data[48]|(lm.data[49]<<8);
  for(unsigned i=0;i<nsh;i++) if(get32(lm.data+sh+i*40+4)==2) {
    unsigned char *s=lm.data+sh+i*40;
    syms=get32(s+16); nsyms=get32(s+20)/16;
    names=get32(lm.data+sh+get32(s+24)*40+16);
    badword(&lm,&lf,sh+i*40+24,nsh);
    badword(&lm,&lf,sh+i*40+36,0);
  }
  assert(syms && names);
  for(unsigned i=0;i<nsyms;i++) {
    uint32_t off=syms+i*16;
    if(!strcmp((char *)lm.data+names+get32(lm.data+off),"Init")) {
      badword(&lm,&lf,off+4,0); // Thumb and executable mapping
      badword(&lm,&lf,off+4,0x2400000A);
      badword(&lm,&lf,off+4,0x24010001);
      badword(&lm,&lf,off+8,0xffffffff);
      badword(&lm,&lf,off,0xffffffff);
    }
  }
  uint32_t crc;
  assert(progFileCrc32(&lf,&crc) && crc==man.loader_crc32);
  prog_image_t image;
  assert(!progInspectImage(&inf,man.address,&image));
  assert(image.sp==0x24050000 && image.reset==0x0800C46D && image.has_version);
  assert(!strcmp(image.name,"STM32H7-GFX-FW") && !strcmp(image.version,"V240322R1"));
  assert(progInspectImage(&inf,0x08020000,&image));
  uint32_t initial=get32(im.data), reset=get32(im.data+4);
  put32(im.data,0x08000000); assert(progInspectImage(&inf,man.address,&image));
  put32(im.data,initial-1); assert(progInspectImage(&inf,man.address,&image));
  put32(im.data,initial);
  put32(im.data+4,reset&~1U); assert(progInspectImage(&inf,man.address,&image));
  put32(im.data+4,0x09000001); assert(progInspectImage(&inf,man.address,&image));
  put32(im.data+4,reset);
  put32(im.data+0x444,0x08020000); assert(progInspectImage(&inf,man.address,&image));
  put32(im.data+0x444,0x08000000);
  inf.size=7; assert(progInspectImage(&inf,man.address,&image));
  inf.size=0x100001; assert(progInspectImage(&inf,man.address,&image));
  inf.size=im.size;
  im.calls=0;
  assert(progFileCrc32(&inf,&crc) && crc==man.crc32);
  int blocks=im.calls;
  int failures[]={1,blocks/2,blocks};
  for(unsigned i=0;i<3;i++) {
    im.fail=failures[i]; im.calls=0; assert(!progFileCrc32(&inf,&crc));
  }
  im.fail=-1; im.data[im.size-1]^=1;
  assert(progFileCrc32(&inf,&crc) && crc!=man.crc32);
  im.data[im.size-1]^=1;
  const char *bad[]={"", "name=a", "other=1", "image=../x @ 0x08000000",
    "image=x @ 0x100000000", "device=STM32F411", "crc32=garbage", "name=a\nname=b"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
    strcpy(config,bad[i]); assert(progParseManifest(config,&man));
  }
  strcpy(config,"# comment\r\n");
  strncat(config,(char *)mm.data,sizeof(config)-strlen(config)-1);
  assert(!progParseManifest(config,&man));
  free(lm.data); free(im.data); free(mm.data);
  puts("PASS: real loader/BIN/manifest, ELF bounds and symbols, injected I/O failures, vectors, CRC corruption");
  return 0;
}
''', encoding='utf-8')
exe = work / 'test_prog.exe'
subprocess.run([a.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root / 'src/ap/modules/prog'),
                str(root / 'src/ap/modules/prog/prog_inspect.c'), str(source), '-o', str(exe)], check=True)
subprocess.run([str(exe), str(a.sd / 'prog/loaders/st/0x483.stldr'),
                str(a.sd / 'prog/fw/h723/app.bin'), str(a.sd / 'prog/fw/h723/fw.txt')], check=True)
