from pathlib import Path
import re, subprocess, argparse, shutil

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description="Run OLED/Flash driver tests with mocked HAL on the host")
parser.add_argument('--cc', default=shutil.which('gcc'), help='Native host C compiler (not arm-none-eabi-gcc)')
args=parser.parse_args()
if not args.cc: parser.error('Provide --cc with a native GCC executable')
gcc=Path(args.cc)
work=root/'build-swd'/'host-tests'
work.mkdir(parents=True,exist_ok=True)
def run(name,preamble,source,tests):
 source=re.sub(r'^#include.*\n','',source,flags=re.M)
 p=work/(name+'.c');p.write_text(preamble+'\n'+source+'\n'+tests,encoding='utf-8')
 exe=work/(name+'.exe')
 subprocess.run([str(gcc),'-std=c11','-w',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

base=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
'''
oled=base+r'''
#define _USE_HW_SSD1315
#define HW_SSD1315_IREF 0x30
#define LCD_WIDTH 128
#define LCD_HEIGHT 64
#define black 0
#define _DEF_I2C1 0
typedef struct {
 bool (*init)(void); bool (*reset)(void);
 void (*setWindow)(int32_t,int32_t,int32_t,int32_t);
 uint16_t (*getWidth)(void);uint16_t (*getHeight)(void);
 bool (*setCallBack)(void (*)(void));
 bool (*sendBuffer)(uint8_t*,uint32_t,uint32_t);
} lcd_driver_t;
static uint8_t commands[256], pixels[1024];
static int count, pages, page, fail_command=-1, fail_page=-1, callbacks;
static void delay(uint32_t ms) {(void)ms;}
static bool i2cBegin(int ch,int rate) {assert(ch==0 && rate==400);return true;}
static bool i2cIsDeviceReady(int ch,int addr) {assert(addr==0x3c);return true;}
static bool i2cWriteByte(int ch,int addr,int ctrl,uint8_t b,int timeout) {
 if(count==fail_command)return false; if((b&0xf8)==0xb0)page=b&7; commands[count++]=b;return true;
}
static bool i2cWriteBytes(int ch,int addr,int ctrl,uint8_t *data,int length,int timeout) {
 assert(ctrl==0x40 && length==128);
 if(pages==fail_page)return false;
 pages++;memcpy(pixels+128*page,data,length);return true;
}
static void done(void) {callbacks++;}
'''
run('oled',oled,(root/'src/hw/driver/lcd/ssd1315.c').read_text(encoding='utf-8'),r'''
int main(void) {
 lcd_driver_t d;assert(ssd1315InitDriver(&d));assert(d.init());
 assert(commands[0]==0xae && commands[1]==0x20 && commands[2]==0x02);
 assert(commands[count-1]==0xaf && pages==8);
 for(int i=0;i<1024;i++)assert(pixels[i]==0);
 uint16_t frame[8192]={0};frame[0]=0xffff;frame[8191]=0xffff;
 count=pages=0;d.setCallBack(done);
 assert(d.sendBuffer((uint8_t*)frame,sizeof(frame),100));
 assert(pages==2 && callbacks==1 && pixels[0]==1 && pixels[1023]==128); /* Only the changed pages. */
 count=pages=0;assert(d.sendBuffer((uint8_t*)frame,sizeof(frame),100));assert(pages==0);
 frame[4000]=0xffff;count=pages=0;fail_page=0;
 assert(!d.sendBuffer((uint8_t*)frame,sizeof(frame),100));
 assert(callbacks==3);
 count=pages=0;fail_page=-1;assert(d.sendBuffer((uint8_t*)frame,sizeof(frame),100));
 assert(pages==8 && pixels[4000/128/8*128+4000%128]==1<<(4000/128%8)); /* A failed write resends everything. */
 count=pages=0;fail_page=-1;fail_command=2;
 assert(!d.init());assert(pages==0);
 puts("PASS OLED: page mode, clear, pixel packing, changed-page updates, callback, I2C error propagation and full resend");
}
''')

src=(root/'src/hw/driver/spi_flash.c').read_text(encoding='utf-8')
consts=sorted(set(re.findall(r'\b(?:HAL_XSPI_[A-Z][A-Z0-9_]*|GPIO_[A-Z][A-Z0-9_]*|RCC_OSPICLKSOURCE_HCLK)\b',src)))
flash=base+'\n'.join('#define '+s+' '+str(i+1) for i,s in enumerate(consts))+'\n'+r'''
#define _USE_HW_SPI_FLASH
#define HW_SPI_FLASH_ADDR 0x90000000
#define HAL_OK 0
#define OCTOSPI1 ((void*)1)
#define GPIOA ((void*)2)
#define GPIOB ((void*)3)
#define GPIOC ((void*)4)
#define portMAX_DELAY 0
#define __HAL_RCC_OSPI_CONFIG(x) ((void)0)
#define __HAL_RCC_OSPI1_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
#define HAL_GPIO_Init(a,b) ((void)0)
#define HAL_GPIO_WritePin(a,b,c) ((void)0)
#define cliAdd(a,b) ((void)0)
#define logPrintf(...) ((void)0)
typedef void cli_args_t;
typedef int SemaphoreHandle_t;
typedef struct {uint32_t Pin,Mode,Pull,Speed,Alternate;} GPIO_InitTypeDef;
typedef struct {uint32_t FifoThresholdByte,MemoryMode,MemoryType,MemorySize,ChipSelectHighTimeCycle,ClockPrescaler,DelayBlockBypass;} XSPI_InitTypeDef;
typedef struct {void *Instance;XSPI_InitTypeDef Init;} XSPI_HandleTypeDef;
typedef struct {uint32_t OperationType,IOSelect,Instruction,InstructionMode,InstructionWidth,AddressMode,AddressWidth,Address,DataMode,DataLength;} XSPI_RegularCmdTypeDef;
typedef struct {uint32_t FlashSize,EraseSectorSize,EraseSectorsNumber,ProgPageSize,ProgPagesNumber;uint8_t device_id[20];} spi_flash_info_t;
static XSPI_RegularCmdTypeDef history[10000],last;
static int ncommand,nprogram,fail_next;
static uint32_t chunks[8], addresses[8], tick;
static int xSemaphoreCreateMutex(void) {return 1;}
static void xSemaphoreTake(int a,int b) {(void)a;(void)b;}
static void xSemaphoreGive(int a) {(void)a;}
static uint32_t millis(void) {return tick++;}
static void delay(uint32_t ms) {tick+=ms;}
static int HAL_XSPI_Init(XSPI_HandleTypeDef *p) {return 0;}
static int HAL_XSPI_Abort(XSPI_HandleTypeDef *p) {return 0;}
static int HAL_XSPI_Command(XSPI_HandleTypeDef *p,XSPI_RegularCmdTypeDef *c,int timeout) {
 if(fail_next){fail_next=0;return 1;} last=*c;history[ncommand++]=*c;return 0;
}
static int HAL_XSPI_Receive(XSPI_HandleTypeDef *p,uint8_t *data,int timeout) {
 if(last.Instruction==0x9f){data[0]=0xef;data[1]=0x40;data[2]=0x19;}
 else if(last.Instruction==5)*data=2;
 else memset(data,0xa5,last.DataLength);
 return 0;
}
static int HAL_XSPI_Transmit(XSPI_HandleTypeDef *p,uint8_t *data,int timeout) {
 chunks[nprogram]=last.DataLength;addresses[nprogram++]=last.Address;return 0;
}
bool spiFlashReset(void);
'''
flash+=(root/'src/common/hw/include/qspi/w25q256jv.h').read_text(encoding='utf-8')
run('flash',flash,src,r'''
int main(void) {
 assert(spiFlashInit());assert(spiFlashIsInit());
 uint8_t data[258];
 assert(spiFlashRead(0x01000000,data,sizeof(data)));
 assert(last.Address==0x01000000 && last.AddressWidth==HAL_XSPI_ADDRESS_32_BITS);
 for(int i=0;i<258;i++)assert(data[i]==0xa5);
 assert(spiFlashWrite(255,data,258));
 assert(nprogram==3 && chunks[0]==1 && chunks[1]==256 && chunks[2]==1);
 assert(addresses[0]==255 && addresses[1]==256 && addresses[2]==512);
 int before=ncommand;
 assert(!spiFlashRead(0xffffffff,data,2));
 assert(!spiFlashWrite(0x02000000,data,1));
 assert(!spiFlashEraseSector(1));
 assert(!spiFlashErase(0,1));
 assert(!spiFlashRead(0,NULL,1));
 assert(ncommand==before);
 assert(spiFlashErase(0x01000000,8192));
 assert(history[ncommand-2].Address==0x01001000);
 fail_next=1;assert(!spiFlashRead(0,data,1));
 spi_flash_info_t info;assert(spiFlashGetInfo(&info));
 assert(info.FlashSize==0x2000000 && info.ProgPageSize==256 && info.EraseSectorSize==4096);
 puts("PASS flash: >16MiB addressing, page split, overflow/alignment rejection, sector erase, HAL errors");
}
''')
