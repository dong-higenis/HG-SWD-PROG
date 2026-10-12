"""Native tests for ESP manifests, programming failure paths and SD packaging."""
from pathlib import Path
import argparse
import importlib.util
import os
import shutil
import subprocess
import uuid

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc', default=shutil.which('gcc'))
p.add_argument('--sd', type=Path, required=True)
p.add_argument('--source', type=Path, required=True)
a = p.parse_args()
if not a.cc:
    p.error('Native GCC required')
env = dict(os.environ)
env['PATH'] = str(Path(a.cc).parent) + os.pathsep + env.get('PATH', '')
work = root / 'build-swd/host-tests/esp'
work.mkdir(parents=True, exist_ok=True)
(work / 'hw_def.h').write_text('#pragma once\n#include <stdint.h>\nuint32_t millis(void);\n')
source = work / 'test_esp.c'
source.write_text(r"""
#include "esp_project.h"
#include "esp_program.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t ticks;
uint32_t millis(void) { return ++ticks; }
static target_chip_t chip;
static unsigned erases, writes, verifies, resets, closes;
static unsigned regions, blanks, rate_changes;
static uint32_t baud, max_baud;
static bool corrupt_link;
static int failure;
static const char *cancel_stage;
static uint32_t capacity;
static uint8_t data[5003];
static esp_loader_port_t port;
esp_loader_port_t *espPortGet(void) { return &port; }
void espPortClose(void) { closes++; }
void espPortStats(esp_port_stats_t *s) { memset(s,0,sizeof(*s));s->baud=baud; }
esp_loader_error_t esp_loader_init_serial(esp_loader_t *l,esp_loader_port_t *p) { (void)l;(void)p;baud=115200;return ESP_LOADER_SUCCESS; }
esp_loader_error_t esp_loader_connect(esp_loader_t *l,esp_loader_connect_args_t *a) {
  (void)l;(void)a;return failure==1?ESP_LOADER_ERROR_TIMEOUT:ESP_LOADER_SUCCESS;
}
esp_loader_error_t esp_loader_connect_with_stub_provider(esp_loader_t *l,esp_loader_connect_args_t *a,
    esp_loader_stub_provider_t provider,void *context) {
  esp_loader_error_t e=esp_loader_connect(l,a);if(e!=ESP_LOADER_SUCCESS)return e;
  const esp_stub_t *stub=provider(l,chip,context);
  if(!stub)return ESP_LOADER_ERROR_UNSUPPORTED_CHIP;
  assert(stub->header.entrypoint&&stub->segment_count==2&&stub->segments[0].size>0);
  return failure==8?ESP_LOADER_ERROR_TIMEOUT:ESP_LOADER_SUCCESS;
}
target_chip_t esp_loader_get_target(esp_loader_t *l) { (void)l;return chip; }
esp_loader_error_t esp_loader_get_security_info(esp_loader_t *l,esp_loader_target_security_info_t *s) {
  (void)l;memset(s,0,sizeof(*s));s->secure_boot_enabled=failure==2;return ESP_LOADER_SUCCESS;
}
esp_loader_error_t esp_loader_flash_detect_size(esp_loader_t *l,uint32_t *s) { (void)l;*s=capacity;return ESP_LOADER_SUCCESS; }
esp_loader_error_t esp_loader_flash_start(esp_loader_t *l,esp_loader_flash_cfg_t *c) {
  (void)l;assert(!c->skip_verify && (c->image_size==68 || c->image_size==5004) && c->block_size==4096);erases++;
  return failure==3?ESP_LOADER_ERROR_TIMEOUT:ESP_LOADER_SUCCESS;
}
esp_loader_error_t esp_loader_flash_write(esp_loader_t *l,esp_loader_flash_cfg_t *c,const void *b,uint32_t n) {
  (void)l;
  unsigned offset=c->image_size==5004?(writes%2)*4096:0;
  unsigned actual=c->image_size==5004?(offset?907:4096):67;
  assert(n==((actual+3)&~3U));assert(!memcmp(b,data+offset,actual));
  if(n>actual)assert(((const uint8_t *)b)[actual]==0xff);
  writes++;return failure==4?ESP_LOADER_ERROR_TIMEOUT:ESP_LOADER_SUCCESS;
}
esp_loader_error_t esp_loader_flash_finish(esp_loader_t *l,esp_loader_flash_cfg_t *c) {
  (void)l;(void)c;verifies++;return failure==5?ESP_LOADER_ERROR_INVALID_MD5:ESP_LOADER_SUCCESS;
}
esp_loader_error_t esp_loader_change_transmission_rate(esp_loader_t *l,uint32_t rate) {
  (void)l;assert(!erases&&!regions&&!writes);baud=rate;rate_changes++;
  return failure==7?ESP_LOADER_ERROR_TIMEOUT:ESP_LOADER_SUCCESS;
}
esp_loader_error_t esp_loader_read_register(esp_loader_t *l,uint32_t address,uint32_t *value) {
  (void)l;assert(address==0x40001000);*value=0x9;
  if(baud>max_baud) {
    if(corrupt_link) { *value=0x8;return ESP_LOADER_SUCCESS; }
    return ESP_LOADER_ERROR_TIMEOUT;
  }
  return ESP_LOADER_SUCCESS;
}
esp_loader_error_t esp_loader_flash_erase(esp_loader_t *l) {
  (void)l;regions++;assert(regions==1);
  return failure==3?ESP_LOADER_ERROR_TIMEOUT:ESP_LOADER_SUCCESS;
}
esp_loader_error_t esp_loader_flash_verify_known_md5(esp_loader_t *l,uint32_t address,uint32_t size,const uint8_t *md5) {
  (void)l;assert(size==0x100000&&address==blanks*0x100000&&regions==1);
  assert(!memcmp(md5,"2fdd6851b32ae931637d4845c037b550",32));blanks++;
  return failure==5?ESP_LOADER_ERROR_INVALID_MD5:ESP_LOADER_SUCCESS;
}
void esp_loader_reset_target(esp_loader_t *l) { (void)l;resets++; }
static bool rd(void *ctx,unsigned image,uint32_t offset,void *b,uint32_t n) {
  (void)ctx;assert(image<2 && offset+n<=sizeof(data) && n<=4096);memcpy(b,data+offset,n);return failure!=6;
}
static bool progress(void *ctx,const char *stage,uint32_t done,uint32_t total) {
  (void)ctx;assert(done<=total);return !cancel_stage || strcmp(cancel_stage,stage);
}
static uint32_t crc32(const uint8_t *p,size_t n) {
  uint32_t c=~0U;while(n--) {c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^(0xEDB88320U&(0U-(c&1)));}return ~c;
}
static esp_project_t project;
static void setup(void) {
  regions=blanks=rate_changes=0;baud=115200;max_baud=921600;corrupt_link=false;
  ticks=erases=writes=verifies=resets=closes=0;failure=0;cancel_stage=NULL;chip=ESP32S3_CHIP;capacity=0x1000000;
  memset(&project,0,sizeof(project));project.chip=ESP_PROJECT_S3;project.baud=115200;project.flash_size=0x1000000;
  project.count=2;project.total=134;
  for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i*17);
  for(int i=0;i<2;i++) { project.images[i].address=0x10000+i*4096;project.images[i].size=67;project.images[i].crc32=crc32(data,67); }
}
static const char *parse(const char *s,esp_project_t *out) {
  char text[1025];assert(strlen(s)<sizeof(text));strcpy(text,s);return espProjectParse(text,out);
}
#define PREFIX "name=ESP test\ndevice=ESP32-S3\nbaud=115200\nflash_size=0x01000000\n"
int main(int argc,char **argv) {
  assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);char text[1025]={0};
  size_t n=fread(text,1,1024,f);assert(n>0 && !ferror(f));fclose(f);
  esp_project_t out;assert(espProjectIsManifest(text));assert(!parse(text,&out));
  assert(out.chip==ESP_PROJECT_S3 && out.count==4 && out.images[2].address==0xe000 && out.flash_size==0x1000000);
  assert(!espProjectIsManifest("name=ESP32-S3\ndevice=STM32H72x/STM32H73x\n"));
  assert(!parse(PREFIX "image=a.bin,0x0,0x43,0x0\n",&out));
  assert(parse(PREFIX "device=ESP32\nimage=a.bin,0x0,0x43,0x0\n",&out));
  assert(parse(PREFIX "image=../a.bin,0x0,0x43,0x0\n",&out));
  assert(parse(PREFIX "image=a.bin,0x1,0x43,0x0\n",&out));
  assert(parse(PREFIX "image=a.bin,0x0,0x1001,0x0\nimage=b.bin,0x1000,0x43,0x0\n",&out));
  assert(parse(PREFIX "image=a.bin,0xfffff000,0x2000,0x0\n",&out));
  assert(parse(PREFIX "image=a.bin,0x0,0x0,0x0\n",&out));
  assert(parse(PREFIX "image=a.bin,0x0,0x43,0x0,trailing\n",&out));
  assert(parse("name=X\ndevice=ESP32-C3\nbaud=12345\nflash_size=0x100000\nimage=a.bin,0x0,0x10,0x0\n",&out));
  assert(!parse("name=X\ndevice=ESP32-C3\nbaud=115200\nflash_size=0x100000\nimage=a.bin,0x0,0x10,0x0\n",&out));
  assert(out.chip==ESP_PROJECT_C3);
  assert(!parse("name=X\ndevice=ESP32-C3\nbaud=auto\nflash_size=0x100000\nimage=a.bin,0x0,0x10,0x0\n",&out));
  assert(out.baud==0);
  assert(!parse("name=X\ndevice=ESP32-C3\nbaud=921600\nflash_size=0x100000\nimage=a.bin,0x0,0x10,0x0\n",&out));
  assert(out.baud==921600);
  assert(!parse("name=X\ndevice=ESP32\nbaud=115200\nflash_size=0x100000\nimage=a.bin,0x1000,0x10,0x0\n",&out));
  assert(out.chip==ESP_PROJECT_32);
  esp_program_input_t input={NULL,rd,progress};esp_program_result_t result;
  setup();assert(espProgramRun(&project,&input,&result));
  assert(erases==2&&writes==2&&verifies==2&&resets==1&&closes==1&&result.verified==134&&result.started);
  setup();chip=ESP32C3_CHIP;assert(!espProgramRun(&project,&input,&result));assert(!erases&&!resets);
  setup();capacity=0x800000;assert(!espProgramRun(&project,&input,&result));assert(!erases&&!resets);
  setup();project.images[1].address=capacity;assert(!espProgramRun(&project,&input,&result));assert(!erases);
  for(int fail=1;fail<=6;fail++) {
    setup();failure=fail;assert(!espProgramRun(&project,&input,&result));assert(!resets&&closes==1&&!result.started);
    if(fail<3)assert(!erases);else assert(erases==1);
    if(fail==5)assert(verifies==1 && result.verified==0);
  }
  setup();project.images[0].crc32^=1;assert(!espProgramRun(&project,&input,&result));assert(!resets&&!verifies);
  setup();cancel_stage="ESP ERASE";assert(!espProgramRun(&project,&input,&result));assert(!erases&&!resets);
  setup();cancel_stage="ESP WRITE";assert(!espProgramRun(&project,&input,&result));assert(erases==1&&!writes&&!resets);
  setup();cancel_stage="ESP RESTART";assert(!espProgramRun(&project,&input,&result));assert(verifies==2&&!resets);
  setup();assert(espProgramProbe(&result));assert(!erases&&!writes&&!resets&&closes==1);
  setup();project.baud=0;assert(espProgramRun(&project,&input,&result));
  assert(result.port.baud==921600&&rate_changes==1&&!result.rate_fallbacks);
  setup();project.baud=0;max_baud=230400;assert(espProgramRun(&project,&input,&result));
  assert(result.port.baud==230400&&rate_changes==3&&result.rate_fallbacks==2&&closes==3);
  setup();project.baud=460800;max_baud=115200;corrupt_link=true;
  assert(espProgramRun(&project,&input,&result));assert(result.port.baud==115200&&rate_changes==2&&result.rate_fallbacks==2);
  setup();project.baud=0;failure=7;assert(espProgramRun(&project,&input,&result));
  assert(result.port.baud==115200&&rate_changes==3);
  setup();project.baud=0;max_baud=0;assert(!espProgramRun(&project,&input,&result));assert(!erases&&!writes&&!resets);
  setup();project.baud=0;failure=4;assert(!espProgramRun(&project,&input,&result));
  assert(erases==1&&writes==1&&rate_changes==1&&closes==1&&!resets);
  esp_program_input_t erase_input={NULL,NULL,progress};
  setup();assert(espProgramErase(ESP_PROJECT_S3,&erase_input,&result));
  assert(regions==1&&blanks==16&&result.verified==capacity&&result.erase_complete&&!result.started&&!resets&&!writes);
  setup();chip=ESP32_CHIP;capacity=0x100000;assert(espProgramErase(ESP_PROJECT_32,&erase_input,&result));
  assert(regions==1&&blanks==1&&result.verified==capacity);
  setup();chip=ESP32C3_CHIP;assert(espProgramErase(ESP_PROJECT_C3,&erase_input,&result));assert(regions==1);
  setup();chip=ESP32C3_CHIP;assert(!espProgramErase(ESP_PROJECT_S3,&erase_input,&result));assert(!regions&&!resets);
  setup();failure=2;assert(!espProgramErase(ESP_PROJECT_S3,&erase_input,&result));assert(!regions&&!resets);
  setup();capacity=0x110000;assert(!espProgramErase(ESP_PROJECT_S3,&erase_input,&result));assert(!regions);
  setup();failure=3;assert(!espProgramErase(ESP_PROJECT_S3,&erase_input,&result));assert(regions==1&&!blanks&&!resets);
  setup();failure=5;assert(!espProgramErase(ESP_PROJECT_S3,&erase_input,&result));
  assert(regions==1&&blanks==1&&!result.verified&&!result.erase_complete&&!resets);
  setup();cancel_stage="ESP CHIP ERASE";assert(!espProgramErase(ESP_PROJECT_S3,&erase_input,&result));assert(!regions&&!resets);
  setup();cancel_stage="ESP BLANK CHECK";assert(!espProgramErase(ESP_PROJECT_S3,&erase_input,&result));assert(regions==1&&!blanks&&!resets);
  setup();cancel_stage="ESP LINK TEST";assert(!espProgramErase(ESP_PROJECT_S3,&erase_input,&result));assert(!regions&&!resets);
  setup();project.total=10006;
  for(unsigned i=0;i<2;i++) { project.images[i].address=0x10000+i*8192;project.images[i].size=5003;project.images[i].crc32=crc32(data,5003); }
  assert(espProgramRun(&project,&input,&result));assert(writes==4&&result.verified==10006&&result.stub);
  setup();failure=8;assert(!espProgramRun(&project,&input,&result));assert(!erases&&!writes&&!resets&&!result.stub);
  setup();failure=8;assert(!espProgramErase(ESP_PROJECT_S3,&erase_input,&result));assert(!regions&&!resets);
  puts("PASS: stub provider checks chip/security before upload; stub failure cannot erase; single chip erase and 1 MiB MD5 checks.");
  puts("PASS: baud negotiation/fallback before writes; full erase/MD5 over 1 and 16 MiB, chip/security/cancel/error guards.");
  puts("PASS: ESP multi-image parser, overlap/overflow/path/chip guards; chip/capacity/security refusal before erase;");
  puts("      transport/read/CRC/MD5 failure, cancel, 4-byte padding, no failed restart, probe without writes.");
}
""", encoding='utf-8')
exe = work / 'test_esp.exe'
includes = [work, root/'src/ap/modules/prog', root/'src/hw/driver', root/'src/lib/esp-serial-flasher/include']
subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror',*[f'-I{x}' for x in includes],str(source),
                str(root/'src/ap/modules/prog/esp_project.c'),str(root/'src/ap/modules/prog/esp_program.c'),
                *[str(root/f'src/lib/esp-serial-flasher/src/stubs/esp_stub_{chip}.c') for chip in ['esp32','esp32s3','esp32c3']],
                '-o',str(exe)],check=True,env=env)
subprocess.run([str(exe),str(a.sd/'prog/fw/esp32s3-rly4-pro/fw.txt')],check=True,env=env)
spec=importlib.util.spec_from_file_location('prepare_esp_sd',root/'tools/prepare_esp_sd.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
package=work/('package-'+uuid.uuid4().hex[:8]);package.mkdir()
assert package.resolve().is_relative_to(work.resolve())
(package/'keep.txt').write_bytes(b'existing')
dest,lines=mod.prepare(a.source,package,'s3','esp32s3','ESP test')
assert (package/'keep.txt').read_bytes()==b'existing'
assert (dest/'ota-reset.bin').read_bytes()==b'\xff'*8192
mod.prepare(a.source,package,'s3','esp32s3','ESP test')
(dest/'firmware.bin').write_bytes(b'keep existing different file')
try:
    mod.prepare(a.source,package,'s3','esp32s3','ESP test')
    raise AssertionError('Overwrite allowed')
except ValueError:
    assert (dest/'firmware.bin').read_bytes()==b'keep existing different file'
try:
    mod.prepare(a.source,package,'c3','esp32c3','Wrong chip')
    raise AssertionError('Wrong chip allowed')
except ValueError:
    assert not (package/'prog/fw/c3').exists()
bad=bytearray((a.source/'firmware.bin').read_bytes());bad[100]^=1
try:
    mod.inspect_image(bad,9)
    raise AssertionError('Corruption allowed')
except ValueError:
    pass
print('PASS: real S3 checksums/SHA/partition MD5, OTA selection reset, wrong chip, overwrite refusal, idempotent package.')
