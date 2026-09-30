# H5-SWD-Prog

**made by Higenis**

STM32H563 기반 **오프라인 SWD 프로그래머**입니다. SSD1315 OLED와 버튼으로 펌웨어를 선택해 STM의 내부 Flash를 기록하거나 전체 삭제합니다.

PC는 프로그래머 펌웨어 다운로드와 SD카드 준비에 사용합니다. 준비가 끝나면 OLED에서 작업할 수 있으며, USART1 CLI로 상태와 상세 로그를 확인할 수 있습니다.

명령 예시는 별도 표기가 없으면 **저장소 루트**에서 실행합니다. 펌웨어 소스는 `firmware/H5_SWD_PRG/`에 있습니다.

## 지원 범위

| 항목 | 현재 구성 |
| --- | --- |
| 프로그래머 | HG-SWD-PROG + STM32H563_CORE, 250MHz |
| 기록 대상 | STM32H723ZGT6, 내부 Flash 1MiB |
| 화면 / 조작 | SSD1315 128×64 I2C OLED / 버튼 4개 |
| 펌웨어 입력 | FAT32 SD카드의 프로젝트별 BIN + `fw.txt` |
| 프로그래밍 방식 | SWD로 H723 RAM에 ST 로더를 올려 실행 |
| 전체 삭제 | 내부 Flash 8개 섹터 삭제 후 1MiB 전체 FF 검증 |
| 진단 | USART1 115200 8N1 CLI, SWD probe/RAM/실행 검사 |

현재 구현은 검사한 `0x483.stldr`와 H723 1MiB 구성을 사용합니다. 다른 MCU, 임의의 로더, 외부 QSPI Flash 기록, SWO 디코딩은 지원하지 않습니다. H563 코어 보드의 W25Q256JV 드라이버는 별도로 유지하며, 타깃의 외부 Flash를 기록하는 기능과는 구분됩니다.

## 빠른 시작

1. 이 프로젝트를 빌드하고 `firmware/H5_SWD_PRG/build-swd/stm32h5-fw.elf`를 **H563 프로그래머**에 다운로드합니다.
2. [연결 방법](#보드-연결)에 따라 H723의 SWCLK, SWDATA, GND를 연결하고 타깃에 USB 전원을 공급합니다.
3. [SD카드 준비](#sd카드-준비)에 따라 로더와 H723용 BIN을 넣습니다.
4. 카드를 H563에 넣고 홈의 `Flash`에서 프로젝트를 선택합니다.
5. 확인 화면에서 SEL을 **놓았다가 1초** 누릅니다.
6. `Flash / Complete`와 `Verified`가 표시되면 기록과 비교가 완료된 것입니다. 타깃은 새 펌웨어로 재시작합니다.

부팅만으로 타깃을 지우거나 기록하지 않습니다. 디버거가 `main`에서 멈춰 있다면 계속 실행해야 OLED와 CLI가 동작합니다.

## 보드 연결

### H563 프로그래머 → H723 타깃

| 프로그래머 단자 | H723 타깃 단자 |
| --- | --- |
| CLK | SWCLK |
| DIO | SWDATA / SWDIO |
| GND | GND |

현재 확인한 구성은 **타깃 USB 별도 전원 + 위 3선 연결**입니다. 프로그래머의 NRESET/SWO와 타깃의 RX/TX/3.3V 단자는 연결하지 않았습니다. 타깃 리셋은 SWD를 통한 소프트웨어 리셋을 사용합니다.

H563 자체 펌웨어를 다운로드하는 ST-Link는 H563의 PA13/PA14 디버그 인터페이스에 연결합니다. 이 연결과 H723으로 나가는 CLK/DIO는 별도입니다.

### CLI / ST-Link VCP

| H563 핀 | 연결할 ST-Link VCP / USB-UART 핀 |
| --- | --- |
| PB15: USART1 TX | RX |
| PB14: USART1 RX | TX |
| GND | GND |

- 설정: **115200 baud, 8 data bits, parity 없음, 1 stop bit, 흐름 제어 없음**.
- USART1의 **TX/RX SWAP이 켜져 있습니다**. 현재 펌웨어는 PB15가 TX, PB14가 RX입니다.
- CLI와 부팅 로그는 COM 포트 터미널에서 확인합니다. SWD 디버거의 Debug Console로 자동 출력되지 않습니다.
- COM 번호는 장치 관리자에서 확인합니다. 이전 실물 연결은 COM19였지만 고정값은 아닙니다.

### 주요 핀맵

| 기능 | H563 핀 / 설정 |
| --- | --- |
| UP / DN | PA6 / PA7, 눌림 LOW |
| SEL / M | PA4 / PA5, 눌림 LOW |
| OLED SCL / SDA | PB6 / PB7, I2C1 400kHz, 7비트 주소 `0x3C` |
| 활동 LED | PC13, LOW 점등 |
| 부저 | PB8, TIM4_CH3 |
| SD CMD / CLK | PB2 / PC12 |
| SD D0 / D1 / D2 / D3 | PB13 / PC9 / PC10 / PC11 |
| SD 감지 | PA8, 삽입 LOW |
| 타깃 SWDIO 출력 / 입력 | PB1 / PC0, 입력 버퍼 경유 |
| 타깃 SWCLK / SWO / NRSET | PB4 / PC1 / PB3 |
| H563 자체 SWDIO / SWCLK | PA13 / PA14 |
| USART1 TX / RX | PB15 / PB14, SWAP 사용 |
| 코어 W25Q256JV NCS / CLK | PB10 / PA3, OCTOSPI1 |
| 코어 W25Q256JV IO0 / IO1 | PC3 / PB0 |
| 코어 W25Q256JV IO2 / IO3 | PC2 / PA1, 현재 단일 데이터선 모드에서 HIGH |
| HSE / LSE | 25MHz / 32.768kHz |

## OLED 메뉴

홈의 제목은 `H5-SWD-Prog`이며 `Flash`, `Erase`, `Info`를 표시합니다. UP/DN으로 이동하고 SEL로 들어갑니다. M은 이전 화면으로 돌아갑니다.

### Flash — 펌웨어 기록

1. `/prog/fw/<id>/fw.txt`가 있는 프로젝트 목록에서 선택합니다. 목록에는 프로젝트 폴더명이 표시됩니다.
2. 이름, BIN 크기, 지울 섹터 수를 확인합니다.
3. SEL을 놓았다가 1초 누릅니다.
4. 파일 검사 → 연결/리셋 → 속도 검사 → 로더 적재 → 삭제 → 기록 → 비교 → 재시작 순서로 진행합니다.

BIN이 차지하는 섹터만 지웁니다. 섹터 크기는 128KiB이며, 마지막 사용 섹터에서 BIN 뒤에 있던 기존 데이터도 삭제됩니다. 현재 887,376바이트 예제는 앞의 7개 섹터를 사용하고 마지막 8번째 섹터를 유지합니다.

성공하면 `Flash / Complete`, `Verified`, 검증 바이트 수와 시간이 표시됩니다. SEL 또는 M으로 파일 목록에 돌아갑니다.

### Erase — 전체 Flash 삭제

확인 화면에서 **내부 Flash 전체 1MiB / 8개 섹터**를 삭제한다는 내용을 확인하고 SEL을 놓았다가 1초 누릅니다.

- `/prog/loaders/st/0x483.stldr`만 필요합니다. BIN과 `fw.txt`는 필요 없습니다.
- `0x08000000..0x080FFFFF`를 지우고 전체 영역을 읽어 `FF`인지 확인합니다.
- 성공하면 Flash를 잠그고 타깃을 **정지 상태로 유지**합니다.
- 완료 화면은 `Erase / Complete`, `Blank OK`, `HALTED`입니다. SEL 또는 M으로 홈에 돌아갑니다.

### Info / SD 상태 / 작업 중 조작

- `Info`: `made by Higenis`를 표시하며 SD카드 없이 열 수 있습니다.
- `No SD card`: 카드 삽입이 필요합니다.
- `SD not ready`: 카드가 감지되었지만 마운트되지 않았습니다. FAT32 형식과 카드 상태를 확인합니다.
- `No firmware`: 등록된 프로젝트가 없습니다. SD 폴더와 `fw.txt`를 확인합니다.
- Flash/Erase 진행 중 **M은 중단 요청**입니다. 실행 중인 로더 호출이 끝난 뒤 다음 단계 전에 멈춥니다.
- 실패·중단 시 타깃 정지를 시도하고 결과를 표시합니다. 정지 확인은 `HALTED`, 세부 원인은 CLI에서 확인합니다.

LED는 대기 중 꺼져 있고 SWD 전송 및 프로그래머의 SD 읽기에 반응해 점멸합니다. 실패나 중단 후에는 일부만 삭제·기록되어 있을 수 있으므로, 정상 펌웨어를 사용하려면 Flash 작업을 다시 완료해야 합니다.

## SD카드 준비

### 폴더 구조

```text
/prog/
├── loaders/
│   └── st/
│       └── 0x483.stldr
└── fw/
    └── h723/
        ├── fw.txt
        └── app.bin
```

`app.bin`은 **H723에 기록할 펌웨어**입니다. 이 프로젝트가 빌드하는 H563용 `stm32h5-fw.bin`과 대상이 다릅니다. 지원 입력은 벡터 테이블부터 시작하는 `0x08000000` 기준 BIN 한 개이며 최대 1MiB입니다.

### 준비 도구

다음은 PC에 연결한 SD카드가 `D:`이고 원본 BIN이 카드 루트에 있을 때의 예입니다.

```powershell
python firmware/H5_SWD_PRG/tools/prepare_h723_sd.py `
  --sd D:/ `
  --image D:/stm32h7-gfx-fw.bin `
  --loader C:/ST/STM32CubeCLT_1.20.0/STM32CubeProgrammer/bin/FlashLoader/0x483.stldr
```

도구는 BIN/로더를 복사하고 CRC가 포함된 `fw.txt`를 생성합니다. 같은 내용의 기존 파일은 재사용하며, 다른 내용의 파일이 있으면 덮어쓰지 않고 중단합니다. 원본 BIN은 유지됩니다.

### fw.txt 예시

아래 CRC는 실물에서 사용한 887,376바이트 예제 BIN의 값입니다. 다른 BIN에는 해당 파일의 CRC32를 사용해야 합니다.

```ini
name=H723 GFX firmware
device=STM32H72x/STM32H73x
algo=/prog/loaders/st/0x483.stldr
image=app.bin @ 0x08000000
crc32=0x2EEECE18
loader_crc32=0x32674EA9
```

- 프로젝트 ID는 영문·숫자·밑줄·하이픈 1~24자이며 최대 16개를 표시합니다.
- 프로젝트를 추가하려면 `/prog/fw/<id>/`에 BIN과 `fw.txt`를 넣습니다.
- `fw.txt`는 BOM 없는 ASCII, 최대 1,024바이트입니다. 위 6개 항목이 모두 필요합니다.
- BIN 파일명은 같은 폴더 안의 파일명으로 지정합니다. ELF 파일이나 임의 주소의 부분 BIN은 지원하지 않습니다.
- SD 파일만 검사하려면 CLI에서 `prog check h723`을 실행합니다. 이 명령은 타깃에 접근하지 않습니다.

## 빌드와 디버깅

VS Code에서는 **`firmware/H5_SWD_PRG` 폴더**를 열어야 해당 폴더의 `.vscode` 작업·디버그 설정을 사용할 수 있습니다. 아래 터미널 명령은 저장소 루트 기준입니다.

필요 도구는 ARM GNU Toolchain, CMake, Ninja입니다. VS Code 디버깅은 C/C++ 및 Cortex-Debug 확장과 ST-Link GDB Server를 사용합니다.

도구가 PATH에 등록되어 있으면:

```powershell
cmake -S firmware/H5_SWD_PRG -B firmware/H5_SWD_PRG/build-swd -G Ninja
cmake --build firmware/H5_SWD_PRG/build-swd --parallel 8
```

현재 PC의 설치 경로를 명시하는 경우:

```powershell
$cmake = 'C:/ST/STM32CubeCLT_1.20.0/CMake/bin/cmake.exe'
& $cmake -S firmware/H5_SWD_PRG -B firmware/H5_SWD_PRG/build-swd -G Ninja `
  -DCMAKE_MAKE_PROGRAM=C:/ST/STM32CubeCLT_1.20.0/Ninja/bin/ninja.exe `
  -DARM_TOOLCHAIN_DIR=C:/work/tools/baram-fw-tools-main/arm_toolchain/arm_gcc/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc.exe
& $cmake --build firmware/H5_SWD_PRG/build-swd --parallel 8
```

`ARM_TOOLCHAIN_DIR`은 CMake 옵션으로 지정할 때 **컴파일러 실행 파일 경로**를 받습니다. 설치 위치가 다르면 예시 경로를 수정합니다.

| 출력 | 용도 |
| --- | --- |
| `firmware/H5_SWD_PRG/build-swd/stm32h5-fw.elf` | H563 다운로드·디버깅 |
| `firmware/H5_SWD_PRG/build-swd/stm32h5-fw.bin` | H563용 바이너리 |
| `firmware/H5_SWD_PRG/build-swd/compile_commands.json` | 편집기 코드 분석 |

F5의 `Debug with STLINK`는 먼저 `build-build` 작업을 실행하고 최신 ELF를 사용합니다. 초기 정지 위치는 `main`입니다. `firmware/H5_SWD_PRG/.vscode/launch.json`의 CubeCLT 경로는 설치 환경에 맞춰야 합니다.

프로젝트 폴더를 이동한 뒤 예전 경로가 표시되면 기존 빌드 폴더를 백업하고 새 경로에서 다시 구성·빌드합니다. 빌드 경로는 `firmware/H5_SWD_PRG/build-swd`이며 이전 `build` 산출물은 사용하지 않습니다.

## 주요 CLI 명령

| 명령 | 동작 |
| --- | --- |
| `uart info` | USART 상태 확인 |
| `sd info` / `fatfs list` | SD 및 파일 상태 확인 |
| `prog list` | 등록된 프로젝트 ID 목록 |
| `prog check h723` | manifest, 로더, BIN, CRC 검사. 타깃 접근 없음 |
| `swd info` | SWD 핀 구성·입력 레벨 확인 |
| `swd probe` | 타깃 ID/CPU 확인. 타깃 정지·리셋·Flash 변경 없음 |
| `swd ramtest` | 타깃 정지 후 DTCM 64바이트 검사·복원 |
| `swd exectest` | ITCM에서 7+35 실행 후 코드·레지스터·실행 상태 복원 |
| `swd release` | SWD 출력 핀 해제 |
| `rtc info` | RTC 상태 확인 |

`probe`, `ramtest`, `exectest`의 기본 클럭 상한은 100kHz이며 `swd probe 50`처럼 10~500kHz를 지정할 수 있습니다. RAM/실행 검사는 타깃을 잠시 정지시키는 진단 기능입니다. 기록과 전체 삭제는 OLED에서 시작합니다.

## 프로그램 방식과 로더

H563은 SWD로 H723의 RAM과 디버그 레지스터에 접근합니다. SD의 ST 로더를 H723 RAM에 적재한 뒤 `Init`, `SectorErase`, `Write`를 호출합니다. 실제 Flash 제어 코드는 H723에서 실행됩니다.

```text
SD 파일 검사 → 타깃 확인/리셋/정지 → SWD 속도 검사 → RAM 로더 적재
  Flash: 필요한 섹터 삭제 → BIN 기록 → BIN 전체 비교 → 타깃 재시작
  Erase: 8개 섹터 삭제 → 1MiB 전체 FF 검사 → Flash 잠금/타깃 정지 유지
```

사용 중인 원본 로더는 CubeCLT 1.20.0의 `STM32CubeProgrammer/bin/FlashLoader/0x483.stldr`입니다. 같은 설치의 `Data_Base/STM32_Prog_DB_0x483.xml`에는 `DeviceID=0x483`, `Name=STM32H72x/STM32H73x`가 명시되어 있습니다. 실물 `swd probe`에서도 `DBGMCU=0x10016483`, 하위 12비트 `DEV_ID=0x483`을 확인했습니다.

`0x483`은 계열 공통 ID이므로 실행 전 Flash 용량 1MiB와 보호 상태도 확인합니다. 로더는 ELF 형식이며 전체 크기 20,748바이트, CRC32 `32674EA9`, 실행 코드 크기 2,020바이트입니다. 파일의 버전·배치·CRC를 고정 검사하므로 다른 `.stldr`로 교체하려면 지원 코드를 검토해야 합니다.

속도는 12,000 → 8,000 → 4,000 → 2,000 → 1,000 → 500 → 100kHz 순으로 RAM 전송을 검사해 선택합니다. `SPEED`의 `clock ceiling`은 설정 상한이며 실제 SWCLK 파형 측정값은 아닙니다. DWT가 동작하지 않으면 TIM5 기반 100kHz 상한을 사용합니다.

기록 데이터는 RAM에 보낸 뒤 되읽어 비교하고, 마지막에는 타깃 Flash와 SD BIN을 전체 비교합니다. 옵션 바이트나 보호 설정을 해제하지 않으며, 실패한 삭제·기록 명령을 자동 재시도하지 않습니다.

<details>
<summary>로더 실행 주소와 검사 방식</summary>

| 항목 | 값 |
| --- | --- |
| 반환 BKPT | `0x24000000` |
| 로더 코드 | `0x24000004..0x240007E8` (끝 주소 제외) |
| Init / Write / SectorErase | `0x2400000B` / `0x24000041` / `0x24000399` |
| 데이터 버퍼 | `0x24002000`, 4KiB |
| 스택 | `0x24004000..0x24008000`, 16KiB |
| Flash 섹터 | 128KiB × 8 |
| Flash 기록 정렬 | 32바이트, 마지막 빈 부분은 `FF` |

- ELF의 주소 0 세그먼트는 `StorageInfo` 메타데이터이며 타깃 RAM에 적재하지 않습니다.
- 로더의 PageSize 4096은 정보 필드입니다. 실제 Flash 기록 정렬은 32바이트입니다.
- 소프트웨어 리셋과 reset vector catch로 코어를 정지시키고 캐시·MPU 상태를 확인합니다.
- 로더 반환 시 R0, PC, DFSR의 BKPT 사유, xPSR/MSP, 스택 표식과 Flash 상태를 검사합니다.
- MEM-AP 연속 전송은 1KiB 경계에서 분할합니다. 기록 데이터는 4KiB씩 전달합니다.
- 속도 후보마다 4KiB RAM 쓰기/읽기를 서로 다른 데이터로 세 번 비교합니다.
- SWD 타이밍은 DWT CPU 사이클을 사용하며 GPIO에 직접 접근합니다. 전송 중 인터럽트는 허용합니다.
- SWD/SD 작업과 CLI 명령은 공통 잠금으로 직렬화합니다. 프로그래밍 중 CLI 명령은 busy로 거부됩니다.

검사한 로더 SHA256:

```text
639a988e15f5e1adebd2939ff79510e759389cbfe41ea4cacc5b961ce491838c
```

실물 예제 BIN은 887,376바이트, CRC32 `2EEECE18`, 초기 SP `0x24050000`, Reset `0x0800C46D`입니다. 내장 버전은 `STM32H7-GFX-FW / V240322R1`입니다.

</details>

## 소스 구조

아래 경로는 `firmware/H5_SWD_PRG/` 내부 기준입니다.

| 경로 | 역할 |
| --- | --- |
| [src/ap/modules/prog/prog.c](firmware/H5_SWD_PRG/src/ap/modules/prog/prog.c) | OLED 메뉴, SD 프로젝트, Flash/Erase 작업, CLI |
| [src/ap/modules/prog/prog_inspect.c](firmware/H5_SWD_PRG/src/ap/modules/prog/prog_inspect.c) | manifest/ELF/BIN/CRC 검사 |
| [src/hw/driver/swd.c](firmware/H5_SWD_PRG/src/hw/driver/swd.c) | SWD GPIO, 타이밍, 진단 CLI |
| [src/hw/driver/swd/](firmware/H5_SWD_PRG/src/hw/driver/swd/) | SWD 전송, DP/AP 접근, 진단, Flash/Erase 엔진 |
| [src/hw/driver/lcd/ssd1315.c](firmware/H5_SWD_PRG/src/hw/driver/lcd/ssd1315.c) | SSD1315 드라이버 |
| [src/hw/hw_def.h](firmware/H5_SWD_PRG/src/hw/hw_def.h) | 사용 장치, 화면 크기, UART, 스레드 설정 |
| [src/bsp/bsp.c](firmware/H5_SWD_PRG/src/bsp/bsp.c) | 시스템 클럭, LSE, 초기화 오류 정보 |
| [tools/](firmware/H5_SWD_PRG/tools/) | SD 준비와 호스트 테스트 |

FreeRTOS, CLI, SDMMC/FatFs, RTC, LED, 버튼, 부저, 코어 Flash 드라이버를 사용합니다. 이전 프로젝트의 CAN 로거, Ethernet/LwIP, LVGL 및 미사용 주변장치 모듈은 제거했습니다.

## 검증 현황

2026-09-30 기준으로 기록된 결과입니다.

| 항목 | 확인 결과 |
| --- | --- |
| H723 식별 | 실물 probe 성공: DPIDR `6BA02477`, CPUID `411FC272`, DEV_ID `483` |
| RAM 검사 | 실물 64바이트 / 4개 패턴 및 RAM·코어 상태 복원 성공 |
| 코드 실행 | 실물 R0=42, BKPT 정지 및 코드·레지스터·MPU·코어 복원 성공 |
| SD 파일 검사 | 실물 로더와 887,376바이트 BIN의 CRC 검사 성공 |
| Flash 기록 | 실물 887,376바이트 전체 기록·비교·재시작 성공 |
| 기록 시간 | 전체 **24초**, 쓰기 **14.737초**, 검증 **3.755초** |
| 고속 전송 안정성 | 초기 ERASE FAULT 1회 후 사용자가 이후 재발 없음을 보고. 원인은 미확정 |
| Erase 단독 기능 | ARM 빌드 및 호스트 모의 테스트 통과. 실물 결과·시간은 아직 미확인 |
| 최신 OLED 메뉴 | 호스트 버튼/파일 모의 테스트와 128×64 렌더링 확인 |
| 코드 스타일 정리 | 빌드·관련 테스트 통과. BIN은 빌드 날짜/시간·assert 줄 번호 외 동일 |

24초는 해당 보드·배선·BIN에서 확인한 결과입니다. 다른 이미지와 연결 환경에서는 시간이 달라질 수 있습니다.

### 호스트 테스트

ARM 컴파일러가 아닌 **PC 실행 파일을 만들 수 있는 GCC**와 Python이 필요합니다. 다음 경로는 현재 PC의 예입니다.

```powershell
$nativeGcc = 'C:/work/tools/baram-fw-tools-main/arm_toolchain/mingw_gcc/bin/gcc.exe'
python firmware/H5_SWD_PRG/tools/test_swd.py --cc $nativeGcc
python firmware/H5_SWD_PRG/tools/test_program.py --cc $nativeGcc
python firmware/H5_SWD_PRG/tools/test_prog_ui.py --cc $nativeGcc
python firmware/H5_SWD_PRG/tools/test_board_drivers.py --cc $nativeGcc
python firmware/H5_SWD_PRG/tools/test_rtc_clock.py --cc $nativeGcc
```

SD의 실물 예제 패키지를 대상으로 로더/BIN 손상과 읽기 실패를 검사할 때:

```powershell
python firmware/H5_SWD_PRG/tools/test_prog.py --cc $nativeGcc --sd D:/
```

`test_prog.py`는 기존 H723 GFX 예제의 벡터·버전 정보도 확인합니다. `test_program.py`와 `test_prog_ui.py`는 설치된 ST 로더를 사용하며 설치 경로가 다르면 `--loader`로 지정합니다. UI 모의 화면은 `firmware/H5_SWD_PRG/build-swd/host-tests/prog-ui/*.pgm`에 생성됩니다.

## 문제 해결

### Error_Handler / SIGTRAP

`__BKPT(0)`에서 멈추는 것은 펌웨어가 초기화 오류를 감지했다는 뜻입니다. Cortex-Debug의 Debug Console에서 다음을 실행합니다.

```text
bt
p bsp_init_stage
p bsp_init_status
```

`BSP_INIT_OSCILLATORS`이면 HSE/PLL, `BSP_INIT_RTC_LSE`이면 LSE 초기화 결과를 확인합니다. 소스를 찾지 못하는 이전 경로 경고는 빌드 경로 문제이며 오류 핸들러 진입 원인과 구분해야 합니다.

### LSE / RTC

현재 코드는 **LSE 전용**, 구동 강도는 `RCC_LSEDRIVE_HIGH`입니다. 사용자는 이 설정으로 기동 성공을 확인했습니다. LSI로 대체하지 않으며 LSE 실패 시 초기화 오류로 멈춥니다.

```text
p bsp_lse_status
p/x bsp_rtc_clock_source
x/wx 0x44020cf0
```

RTC 소스가 바뀌면 HAL이 backup domain을 초기화할 수 있습니다. RTC 분주는 32.768kHz에서 128×256을 사용합니다.

### CLI가 출력되지 않음

1. H563가 오류 핸들러나 디버거의 `main` 중단점에서 멈춰 있지 않은지 확인합니다.
2. VCP COM 포트와 115200 8N1 설정을 확인합니다.
3. PB15 TX → 어댑터 RX, PB14 RX ← 어댑터 TX, 공통 GND를 확인합니다.
4. 디버거에서 `p uart_debug`와 `x/s log_buf_boot.buf`로 초기화 상태와 저장된 로그를 확인합니다.

### Flash / Erase 실패

CLI의 `PROGRAM` 또는 `ERASE`, `STATUS`, `FAIL IO`, `CORE`, `SPEED`, `TIMING` 줄을 확인합니다.

- `SR=previous/unavailable`: 이전 작업의 값일 수 있으므로 이 값만으로 원인을 판단하지 않습니다.
- `flash status error=1`: Flash 상태 레지스터 검사에서 실패했습니다.
- `FAIL IO`: 실패한 접근 종류·주소·ACK와 당시 마지막 CTRL_STAT를 표시합니다.
- `CORE`의 `held halted`: 타깃 정지를 확인했습니다. `state not confirmed`이면 정지를 확인하지 못했습니다.

통신 실패 시 100kHz로 다시 연결해 정지를 시도하며, 실패 순간의 진단 정보는 보존합니다. 현재 NRST 미연결 구성에서는 SWD 연결 자체가 차단된 타깃의 복구가 제한됩니다.

## 코드 스타일

- 함수: 모듈 접두사 + lowerCamelCase (`progDraw`, `swdProgramRead32`). CLI 함수는 `cliProg` 형태.
- 변수·구조체 필드: snake_case. 타입은 snake_case + `_t`, 상수·매크로는 UPPER_SNAKE_CASE.
- 들여쓰기 2칸, 탭 금지, 중괄호는 다음 줄, 줄 길이는 120자 기준.
- 짧은 함수·조건문·반복문도 펼치고 중괄호를 사용합니다. 여러 실행문과 구조체 필드를 한 줄에 모으지 않습니다.
- HAL 콜백, 인터럽트, `Error_Handler` 등 외부에서 이름으로 연결하는 함수는 지정된 이름을 유지합니다.
- [.clang-format](firmware/H5_SWD_PRG/.clang-format)을 사용합니다. VS Code C/C++ 확장의 clang-format 23.1.0으로 확인했습니다.
- HAL/CMSIS/FreeRTOS/FatFs 등 외부 라이브러리와 생성된 파일에는 일괄 포맷을 적용하지 않습니다.

## 설계 자료

보드 구성은 `hg-swd-prog_V1.pdf`, `stm32h563_core_v1.pdf`와 실물 연결 정보를 기준으로 했습니다. OLED 컨트롤러는 SSD1315입니다. 프로그래밍 방식은 Baram STM32H7 GFX 프로젝트의 SWD 프로그래밍 문서와 구현을 참고했으며, 이 프로젝트는 OLED·버튼 기반 UI를 사용합니다.
