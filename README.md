# H5-SWD-Prog

**SD카드의 펌웨어를 OLED에서 선택해 기록하는 STM32H562 기반 오프라인 SWD 프로그래머.**  
made by **Higenis**

처음 한 번 PC에서 프로그래머 펌웨어와 SD카드를 준비하면, 이후에는 보드의 버튼만으로 타깃을 기록하거나 삭제할 수 있습니다. 화면은 SSD1315 128×64 OLED를 사용합니다. **STM32H723ZGT6 내부 Flash 1MiB** 기록을 지원하며, 같은 5핀 커넥터를 사용하는 **ESP32·ESP32-S3·ESP32-C3 UART 기록 기능**도 추가했습니다. Auto 모드에서는 칩별로 지정한 프로젝트를 감지 결과에 맞춰 기록합니다. ESP stub과 자동 기록의 실제 통신 속도는 연결한 보드에서 확인합니다.

[동작 방식](#동작-방식) · [빌드](#1-프로그래머-펌웨어-빌드) · [H562 다운로드](#2-h562에-프로그래머-펌웨어-다운로드) · [SD카드 준비](#3-sd카드-준비) · [H723 기록](#4-h723-연결과-펌웨어-기록) · [ESP 기록](#5-esp-펌웨어-기록) · [자동 기록](#6-auto-자동-기록) · [메뉴](#메뉴와-버튼) · [CLI](#cli로-상태-확인)

## 동작 방식

```text
PC에서 준비                       보드에서 실행

이 저장소 빌드 ── ST-Link ──▶ H562 프로그래머
                                  │ OLED에서 프로젝트 선택
SD카드 [로더 + H723용 BIN] ────────▶│
                                  └── SWD ──▶ H723 타깃
                                              삭제 → 기록 → 검증 → 재시작
```

H562은 SD카드에서 BIN과 로더를 읽습니다. **로더는 H723의 RAM에서 실행되는 작은 Flash 기록 프로그램**입니다. H562이 SWD로 로더와 데이터를 전달하면 H723이 자신의 내부 Flash를 지우고 기록합니다. 기록 후에는 Flash를 다시 읽어 SD카드의 BIN과 비교하고, 성공하면 새 펌웨어를 실행합니다.

ESP 프로젝트를 선택하면 H562이 EN·BOOT를 제어해 ESP의 ROM 다운로드 모드로 진입하고, UART로 여러 BIN을 지정 주소에 기록합니다. 칩에 맞는 RAM flasher stub을 올린 뒤 4KiB 블록으로 전송하며, 각 이미지의 MD5 검증이 끝나면 ESP를 리셋해 실행합니다. ESP에는 ST 로더 파일이 필요 없습니다.

파일의 역할은 다음과 같습니다.

| 파일 | 기록할 보드 | 용도 |
| --- | --- | --- |
| 이 저장소를 빌드한 `stm32h5-fw.elf` | **H562 프로그래머** | OLED, SD카드, SWD 프로그래밍 기능 실행 |
| SD카드에 넣는 `app.bin` | **H723 타깃** | 타깃에서 실행할 사용자 펌웨어 |
| ESP 프로젝트의 여러 BIN | **ESP 타깃** | 부트로더·파티션 테이블·앱 등, 파일마다 지정된 주소에 기록 |

## 1. 프로그래머 펌웨어 빌드

### 준비 도구

| 도구 | 용도 |
| --- | --- |
| CMake + Ninja | 빌드 구성 및 실행 |
| Arm GNU Toolchain (`arm-none-eabi-gcc`) | H562 펌웨어 컴파일 |
| STM32CubeCLT 1.20.0 | ST-Link 디버그 서버, CubeProgrammer, H723 로더 |
| VS Code + Cortex-Debug 확장 | 빌드한 펌웨어 다운로드 및 디버깅 |
| Python 3.9 이상 | SD카드 준비 스크립트 실행 |

터미널에서 `cmake`, `ninja`, `arm-none-eabi-gcc`를 실행할 수 있도록 각 도구의 실행 파일 경로를 `PATH`에 등록합니다.

### 빌드 명령

저장소 루트에서 PowerShell을 열고 실행합니다.

```powershell
cd firmware/H5_SWD_PRG
cmake -S . -B build-swd -G Ninja
cmake --build build-swd --parallel 8
```

**이후 명령도 모두 `firmware/H5_SWD_PRG/` 폴더에서 실행합니다.** 빌드 결과는 `build-swd/`에 생성됩니다.

| 결과물 | 용도 |
| --- | --- |
| `build-swd/stm32h5-fw.elf` | ST-Link 다운로드 및 소스 디버깅 |
| `build-swd/stm32h5-fw.bin` | H562 펌웨어의 바이너리 이미지 |

소스를 수정한 뒤에는 마지막 빌드 명령만 다시 실행하면 됩니다. 현재 빌드와 디버그 설정은 `build-swd/`를 사용합니다.

<details>
<summary>컴파일러를 찾지 못하거나, 저장소 경로를 옮긴 경우</summary>

컴파일러가 PATH에 없다면 설치된 **gcc 실행 파일**을 지정합니다. 아래 경로는 본인의 설치 위치로 바꿉니다.

```powershell
cmake -S . -B build-swd -G Ninja `
  -DARM_TOOLCHAIN_DIR="C:/toolchains/arm-gnu/bin/arm-none-eabi-gcc.exe"
cmake --build build-swd --parallel 8
```

저장소 폴더명을 바꾸거나 이동했다면 기존 CMake 캐시에 이전 절대 경로가 남을 수 있습니다. 필요한 산출물을 보관한 뒤 `build-swd/`를 삭제하고 처음부터 구성·빌드합니다. 이 폴더는 빌드 시 다시 생성됩니다.

</details>

## 2. H562에 프로그래머 펌웨어 다운로드

### ST-Link 연결

H562의 디버그 단자에 ST-Link를 연결하고 보드에 전원을 공급합니다.

| ST-Link 신호 | H562 연결 |
| --- | --- |
| SWDIO | PA13 |
| SWCLK | PA14 |
| GND | GND |

### VS Code에서 다운로드

1. VS Code에서 저장소의 **`firmware/H5_SWD_PRG/` 폴더를 엽니다.** 이 폴더에 `.vscode` 설정이 있습니다.
2. [launch.json](firmware/H5_SWD_PRG/.vscode/launch.json)의 `windows` 항목을 확인합니다. 기본 설치 경로는 `C:/ST/STM32CubeCLT_1.20.0/`입니다.
3. 실행 및 디버그에서 **`Debug with STLINK`**를 선택하고 **F5**를 누릅니다.
4. 자동으로 빌드하고 `build-swd/stm32h5-fw.elf`를 H562에 다운로드합니다.
5. `main()`에서 멈추면 **F5로 계속 실행**합니다.
6. OLED에 Auto 대기 화면(`Auto`, `NO SD`/`SETUP`/`READY`)이 표시되는지 확인합니다. M을 누르면 `Auto setup / Manual / Storage / Network / Info` 메뉴가 열립니다.

<details>
<summary>터미널에서 다운로드하기 — STM32CubeProgrammer CLI</summary>

ST-Link가 H562에 연결된 상태에서 실행합니다. 디버깅 중이라면 먼저 디버그 세션을 종료합니다.

```powershell
& 'C:/ST/STM32CubeCLT_1.20.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe' `
  -c port=SWD `
  -w build-swd/stm32h5-fw.elf `
  -v -rst
```

ELF에 포함된 주소로 다운로드하고 검증한 뒤 H562을 리셋합니다. 설치 경로가 다르면 실행 파일 경로를 수정합니다.

</details>

## 3. SD카드 준비

**FAT32 SD카드**, **H723용 BIN**, **ST의 H723 로더**를 준비합니다. BIN은 시작 주소 `0x08000000`에 맞춰 빌드한 벡터 테이블 포함 이미지여야 하며, 최대 크기는 1MiB입니다.

다음은 SD카드가 `D:`일 때의 예입니다. `--image`를 실제 **H723용 BIN 경로**로 바꿔 실행합니다.

```powershell
python tools/prepare_h723_sd.py `
  --sd 'D:/' `
  --image 'C:/path/to/h723/app.bin' `
  --loader 'C:/ST/STM32CubeCLT_1.20.0/STM32CubeProgrammer/bin/FlashLoader/0x483.stldr'
```

스크립트는 BIN과 로더를 검사하고, 파일을 복사하며, CRC32를 계산해 `fw.txt`를 생성합니다. 현재 스크립트는 **CubeCLT 1.20.0에서 확인한 로더 파일**을 사용합니다.

```text
SD카드 루트/
└── prog/
    ├── loaders/
    │   └── st/
    │       └── 0x483.stldr       H723 Flash 로더
    └── fw/
        └── h723/                OLED에 표시할 프로젝트 ID
            ├── fw.txt           이름, 파일 경로, 기록 주소, CRC32
            └── app.bin          H723에 기록할 펌웨어
```

**BIN과 `fw.txt`는 한 쌍입니다.** BIN을 바꿀 때는 CRC32도 갱신해야 합니다. 스크립트는 기존 파일과 내용이 다르면 덮어쓰지 않고 중단하므로, SD카드의 기존 `prog/` 폴더를 PC에 옮겨 보관한 뒤 다시 실행합니다.

여러 펌웨어를 추가하려면 PC에 빈 폴더를 만들고 `--sd`에 그 폴더 경로를 지정해 새 패키지를 생성합니다. 생성된 `prog/fw/h723/`을 SD카드의 `prog/fw/<새 프로젝트 ID>/`로 복사하면 됩니다. 각 폴더의 `fw.txt`와 `app.bin`을 함께 유지하며 로더는 공유합니다. 프로젝트 ID에는 영문·숫자·`_`·`-`를 사용하며 최대 24자입니다. SD에는 칩 구분 없이 최대 16개까지 둘 수 있습니다. 넘으면 프로젝트 ID 알파벳순(대문자가 소문자보다 앞)으로 앞의 16개만 표시되고, Installed bin에 `16+ on SD`가 표시됩니다.

등록된 H723 펌웨어는 [HG-SWD FW Hub](https://can-dubu.github.io/hg-swd-fw-hub/)에서 이 구조 그대로 SD 패키지(zip)로 받을 수도 있습니다. 로더는 라이선스 때문에 포함하지 않으므로 직접 복사합니다.

OLED는 각 프로젝트를 **`fw.txt`의 `device=` 값으로 칩별로 묶어** 보여 줍니다. 묶는 기준은 FW Hub의 `registry/chips/*.toml`과 같습니다.

| `device=` | OLED 칩 | Hub 칩 ID |
| --- | --- | --- |
| `STM32H72x/STM32H73x` | H723 | `stm32h723` |
| `ESP32` | ESP32 | `esp32` |
| `ESP32-S3` | ESP32-S3 | `esp32s3` |
| `ESP32-C3` | ESP32-C3 | `esp32c3` |

그 밖의 `device=` 값은 **Storage → Installed bin**의 `Unknown`에만 표시하며 기록할 수 없습니다.

Hub의 SD 패키지에는 `prog/fw/<id>/release.txt`(`schema=1`, `id`, `chip`, `version`, `date`)가 함께 들어 있습니다. 이 파일은 표시와 칩 구분에만 쓰며, 기록 여부와 기록 내용은 `fw.txt`가 정합니다.

- `chip=`은 그 칩의 `device=`가 `fw.txt`와 같을 때만 사용합니다. 다르면 `Chip mismatch`, 펌웨어가 모르는 칩이면 `Unknown chip`으로 `Unknown`에 묶이고 기록할 수 없습니다.
- `release.txt`가 없거나 쓸 수 없으면(1KiB 초과, `schema`가 1이 아님, 같은 키 중복) `device=`로 판단합니다. 모르는 키, `#` 주석, 빈 줄, CRLF는 허용합니다.
- `version=`이 `x.y.z` 또는 `x.y.z-tag` 형식(31자 이하)이면 Installed bin 상세 화면 아래쪽에 `v1.0.0`처럼 표시합니다.

복사가 끝나면 SD카드를 안전하게 꺼내 H562 프로그래머에 넣습니다.

## 4. H723 연결과 펌웨어 기록

### 타깃 연결

| H5-SWD-Prog 출력 단자 | H723 타깃 단자 |
| --- | --- |
| CLK | SWCLK |
| DIO | SWDATA / SWDIO |
| GND | GND |

현재 사용 구성은 **H723에 USB로 별도 전원을 공급하는 3선 연결**입니다. NRESET/SWO, 타깃의 RX/TX/3.3V는 연결하지 않습니다. 타깃의 리셋은 SWD를 통해 수행합니다.

### 타깃 전원 공급 (3V_OUT / 5V_OUT)

7핀 단자의 **3V_OUT**(PB5)과 **5V_OUT**(PA9)으로 타깃에 전원을 줄 수 있습니다. 메뉴의 **Power**에서 설정합니다.

| 항목 | 값 | 동작 |
| --- | --- | --- |
| **3.3V out** | ON / OFF | 3V_OUT 사용 여부 |
| **5V out** | ON / OFF | 5V_OUT 사용 여부. 3.3V와 따로 켜고 끄며, 둘 다 켤 수도 있습니다 |
| **Supply** | Job / Always | **Job**: 작업(Auto 감지·기록, Manual Flash·Erase)이 타깃에 접근하기 직전에 켜고 250ms 기다린 뒤 진행하며, 결과 화면이 뜰 때 끕니다. **Always**: 켜 둔 출력을 작업과 관계없이 계속 유지합니다 |

- SEL을 누르면 값이 바뀌고 바로 SD의 `/prog/power.cfg`에 저장됩니다. Auto 설정처럼 **SD카드에 속한 설정**이라 다른 카드를 넣으면 그 카드의 설정을 읽고, 파일이 없으면 모두 OFF입니다. 카드를 빼도 현재 출력은 유지됩니다.
- SD가 없으면 바꾼 값은 적용만 되고 저장되지 않으며, 화면 아래에 `Not saved`가 표시됩니다.
- 출력이 실제로 켜져 있으면 대기 화면 왼쪽 위에 `3.3V`, `5V`, `3.3V & 5V` 배지가 표시됩니다.
- 전원을 켜거나 리셋하는 동안에는 하드웨어 풀다운으로 두 출력이 모두 꺼져 있습니다.
- **타깃 전압을 확인한 뒤 켜십시오.** 3.3V 타깃에 5V out을 켜면 타깃이 손상될 수 있습니다.

### OLED에서 Flash 실행

1. 대기 화면에서 **M**을 눌러 메뉴를 열고 **Manual → Flash**를 선택합니다.
2. 칩 목록(`H723`, `ESP32`, `ESP32-S3`, `ESP32-C3`)에서 칩을 고릅니다. 오른쪽 숫자는 기록할 수 있는 프로젝트 수입니다.
3. 그 칩의 프로젝트가 이름순으로 표시됩니다. UP/DN으로 고르고 SEL을 누릅니다.
4. 확인 화면에서 펌웨어 이름과 크기를 확인합니다.
5. **SEL을 놓았다가 1초 동안 눌러** 기록을 시작합니다.
6. 진행률과 경과 시간을 확인합니다. 데이터 처리 중에는 활동 LED가 깜빡입니다.
7. **`Flash / Complete`와 `Verified`**가 표시되면 완료입니다. H723은 새 펌웨어로 재시작합니다.

기록 순서는 **파일 검사 → 타깃 연결 → 삭제 → 기록 → 비교 검증 → 재시작**입니다. BIN이 사용하는 128KiB 섹터를 지우므로 마지막 사용 섹터에서 BIN 뒤에 있던 데이터도 삭제됩니다. 사용하지 않는 나머지 섹터는 유지합니다.

## 5. ESP 펌웨어 기록

### 5핀 연결

ESP 모드에서는 같은 커넥터를 아래 용도로 사용합니다. **프로젝트 종류에 맞춰 타깃과 케이블을 바꿉니다.** ESP 전원은 USB 등으로 별도 공급합니다.

| H5-SWD-Prog 단자 | ESP 연결 | 동작 |
| --- | --- | --- |
| CLK | UART 다운로드 RX | 프로그래머의 UART7 송신 |
| SWO | UART 다운로드 TX | PC1 입력 버퍼를 통한 소프트웨어 수신 |
| nRESET | EN | 리셋 제어 |
| DIO | BOOT | 다운로드 모드 선택: ESP32/S3는 GPIO0, C3는 GPIO9 |
| GND | GND | 공통 접지 |

ESP 보드는 해당 칩의 정상적인 부트 스트랩 구성을 갖춰야 합니다. 기존 CLI의 USART1 배선은 그대로 사용할 수 있습니다. **ESP는 115200bps로 연결한 뒤, 기록 전에 최대 921600bps까지 통신을 검사합니다.** 검사에 실패하면 460800 → 230400 → 115200 순서로 낮춰 재연결합니다. 실제 선택된 속도는 CLI의 `UART RX`, 재시도 횟수는 `LINK`에 표시됩니다. CLI 자체 속도는 115200으로 유지합니다.

### SD 프로젝트 만들기

`firmware/H5_SWD_PRG/`에서 실행합니다. 아래 예는 HG-ESP32-S3-RLY4-PRO 빌드 결과 폴더와 D: SD카드 기준입니다.

```powershell
python tools/prepare_esp_sd.py `
  --source 'C:/path/to/HG-ESP32-S3-RLY4-PRO' `
  --sd 'D:/' `
  --chip esp32s3 `
  --project esp32s3-rly4-pro `
  --name HG-ESP32-S3-RLY4-PRO
```

생성 위치는 `/prog/fw/esp32s3-rly4-pro/`입니다. 기존 `h723` 프로젝트와 ST 로더는 유지합니다. 다른 ESP 프로젝트는 `--chip`을 `esp32` 또는 `esp32c3`로, 프로젝트 ID와 이름을 함께 바꿉니다.

| 파일 | S3 기록 주소 | 역할 |
| --- | --- | --- |
| `bootloader.bin` | `0x00000000` | 제공된 부트로더 |
| `partitions.bin` | `0x00008000` | 제공된 파티션 테이블 |
| `ota-reset.bin` | `0x0000E000` | 생성된 FF 이미지. OTA 선택 정보를 초기화해 app0 부팅 |
| `firmware.bin` | `0x00010000` | 제공된 앱 |
| `fw.txt` | SD에서만 사용 | 대상 칩, Flash 크기, 각 BIN의 주소·크기·CRC32 |

이 패키지는 **16MiB Flash**를 요구합니다. 기록하는 영역의 4KiB 섹터만 지우며, NVS·FFat·app1 영역은 유지합니다. OTA 선택 정보 초기화로 기존 OTA 슬롯 선택은 해제됩니다.

준비 스크립트는 BIN의 칩 ID·체크섬·SHA256과 파티션을 검사합니다. 기본 배치는 파티션 `0x8000`, 앱 `0x10000`이며, 다른 배치의 프로젝트는 스크립트 확장이 필요합니다. 기존 파일 내용이 다르면 덮어쓰지 않으므로 새 프로젝트 ID를 쓰거나 기존 프로젝트를 백업한 뒤 준비합니다.

### 연결 확인 후 기록

H562에 새 펌웨어를 다운로드하고 계속 실행한 뒤, SD를 넣고 CLI에서 확인합니다.

```text
esp info
prog check esp32s3-rly4-pro
esp probe
```

- `prog check`: 모든 이미지의 크기·CRC와 칩 헤더 검사. 타깃에 접근하지 않습니다.
- `esp probe`: **ESP를 리셋해 ROM 다운로드 모드로 진입**하고 칩·Flash 용량을 확인합니다. Flash는 변경하지 않으며 다운로드 모드로 남습니다.
- `UART RX`의 `framing`, `overflow`, `late`는 각각 프레임 오류, 수신 버퍼 넘침, 샘플링 지연 횟수입니다. 실패하면 이 줄과 실패 단계를 확인합니다.

연결이 확인되면 **Manual → Flash → ESP32-S3 → HG-ESP32-S3-RLY4-PRO → SEL을 놓았다가 1초 누르기**로 기록합니다. 칩 종류와 실제 Flash 용량을 확인한 뒤 각 BIN을 삭제·기록·MD5 검증합니다. 모두 통과하면 `Verified`를 표시하고 ESP를 재시작합니다. 실패하거나 취소하면 자동으로 앱을 재시작하지 않습니다.

### 속도 설정

ESP32/S3/C3의 RAM stub은 프로그래머 펌웨어에 포함되어 있습니다. 별도 SD 로더 파일은 필요 없습니다. 기록 결과의 `LOADER`에 `RAM stub / 4096-byte blocks`가 표시됩니다. Stub 업로드나 연결 검사에 실패하면 Flash 작업 전에 중단합니다.

새로 만든 `fw.txt`의 기본값은 `baud=auto`입니다. 기존 프로젝트도 이 줄을 `baud=auto`로 바꾸면 최대 921600bps에서 검사를 시작합니다. `115200`, `230400`, `460800`, `921600`을 쓰면 해당 값을 상한으로 사용합니다. 준비 스크립트에서는 `--baud 460800`처럼 지정할 수 있습니다.

속도 선택은 삭제·기록 전에 끝납니다. 기록 중 통신 오류가 나면 작업을 중단하며, 속도를 바꿔 자동으로 재기록하지 않습니다. 짧은 연결 검사를 통과해도 실제 배선에서 오류가 나면 `baud` 상한을 낮추고 `UART RX` 로그를 확인합니다. 921600bps의 선로 전송 한도는 115200bps의 8배지만, 전체 작업 시간에는 SD 검사·Flash 삭제·MD5 검증도 포함됩니다.

### ESP 전체 삭제

**Manual → Erase → ESP32-S3 → SEL을 놓았다가 1초 누르기**로 시작합니다. ESP32/C3는 목록에서 해당 칩을 선택합니다. 칩 종류와 실제 Flash 용량을 확인한 뒤 RAM stub의 **전체 칩 삭제 명령 한 번**으로 지웁니다. 이후 1MiB씩 MD5를 모두 `FF`인 데이터와 비교합니다. 칩 삭제 명령 중에는 화면 진행률 갱신과 취소 처리가 명령 완료까지 대기합니다. 취소하려면 M을 누른 채 기다립니다.

**전체 삭제는 앱·부트로더·파티션 테이블·NVS 설정·OTA 데이터·파일시스템까지 지웁니다.** ESP 전체 삭제에는 SD카드가 필요 없습니다. 완료하면 `Blank OK`와 `STUB`가 표시되며 RAM stub 다운로드 상태로 남습니다. 다시 실행하려면 Manual → Flash에서 펌웨어를 기록합니다.

암호화·보안 부팅용 프로비저닝과 eFuse 변경은 지원하지 않습니다.

## 6. Auto 자동 기록

Auto는 **SEL 한 번으로 칩 감지 → 저장한 프로젝트 선택 → 파일 검사 → 기록·검증 → 재시작**을 진행합니다. 타깃은 H723용 지원 경로와 ESP32·ESP32-S3·ESP32-C3입니다. 같은 칩을 쓰는 서로 다른 제품은 자동 구분하지 않으므로 작업할 제품의 프로젝트를 먼저 지정합니다.

### 칩별 프로젝트 지정

1. SD카드를 넣고 대기 화면에서 **M → Auto setup**을 엽니다. 아무 칩도 지정되지 않은 카드라면 대기 화면이 `SETUP`으로 표시되며 SEL로 바로 열 수 있습니다.
2. `H723`, `ESP32`, `ESP32-S3`, `ESP32-C3` 중 설정할 항목을 고릅니다. 각 줄 오른쪽에 지정된 프로젝트 ID가 표시됩니다.
3. 해당 칩의 기록 가능한 프로젝트만 **`fw.txt`의 이름순**으로 표시됩니다. 현재 지정된 항목에는 `*`가 붙고, 목록은 그 항목에서 시작합니다. 긴 이름은 선택한 상태에서 옆으로 스크롤됩니다.
4. UP/DN으로 선택하고 **SEL로 저장**합니다. `(None)`은 해당 칩의 지정을 해제합니다.
5. 미지정 항목은 `---`로 표시됩니다. 대기 화면 아래쪽 칩 표시(`H723 E32 S3 C3`)도 지정된 칩만 채워서 보여줍니다.

예를 들어 H723에는 `H723 GFX firmware`, ESP32-S3에는 `HG-ESP32-S3-RLY4-PRO`를 지정합니다. 설정은 SD의 `/prog/auto0.cfg`, `/prog/auto1.cfg`에 번갈아 저장하며 CRC와 순번으로 유효한 최신 설정을 읽습니다. SD카드를 교체하면 그 카드의 설정을 사용합니다. 이 파일들은 OLED에서 관리합니다.

### 연속 작업

1. 전원을 켜면 Auto 대기 화면이 바로 표시됩니다. `READY`인지 확인합니다.
2. 타깃을 연결하고 전원을 공급합니다.
3. **SEL**을 눌러 감지·기록을 시작합니다. 추가 확인 없이 지정한 펌웨어가 기록됩니다.
4. 결과 화면에서 큰 글씨의 **`PASS`/`FAIL`**을 확인합니다. 성공은 짧은 부저, 실패는 긴 부저로 알립니다. `FAIL` 아래에는 실패 원인이 표시됩니다.
5. **다음 보드로 교체하고 결과 화면에서 SEL**을 누르면 다시 감지·기록합니다. M은 대기 화면으로 돌아갑니다.

대기 화면과 결과 화면 오른쪽의 `OK`/`NG`는 전원을 켠 뒤의 Auto 성공·실패 횟수입니다(최대 999 표시). M으로 취소한 작업은 세지 않으며, 전원을 끄면 초기화됩니다.

완료 후에는 자동으로 다시 감지하거나 리셋하지 않습니다. 버튼을 누른 채 유지해도 재기록하지 않으며, 놓았다가 다시 눌러야 다음 작업이 시작됩니다. 현재 5핀에는 별도의 연결 감지선이 없어 보드 교체 확인을 SEL로 대신합니다.

감지는 SWD부터 시도합니다. SWD 응답이 전혀 없을 때 ESP의 EN·BOOT를 제어해 ROM 다운로드 모드에서 칩을 확인합니다. SWD에 응답한 미지원 칩이나 통신 오류는 ESP로 넘기지 않고 중단합니다. ESP 감지 과정에서는 타깃이 리셋됩니다. 감지 결과와 프로젝트 칩이 다르거나 파일·용량 검사에 실패하면 기록하지 않습니다. STM32 지원은 현재 H723용 로더 범위이며, 모든 H7을 지원하는 것은 아닙니다.

자동 기록은 각 BIN이 사용하는 영역을 삭제합니다. 전체 초기화가 필요하면 별도의 Manual → Erase 메뉴를 사용합니다. CLI의 `AUTO`에서 감지한 칩과 선택한 프로젝트 ID를, `UART RX`에서 실제 ESP 속도를 확인할 수 있습니다.

## 7. 네트워크와 Hub 설치

ESP32-S3 + LTE 보드(HG-ESP32S3-LTE)를 네트워크 모듈로 연결하면, [HG-SWD FW Hub](https://can-dubu.github.io/hg-swd-fw-hub/)의 펌웨어를 PC 없이 SD카드에 설치할 수 있습니다. ESP는 Wi-Fi/LTE 연결과 HTTPS 다운로드만 하고, Hub 목록 해석과 SD 기록은 H562이 합니다. ESP 펌웨어와 통신 규격은 [firmware/ESP32S3_NET](firmware/ESP32S3_NET/)에 있습니다([PROTOCOL.md](firmware/ESP32S3_NET/PROTOCOL.md)).

### 연결

두 보드는 전원을 따로 공급하고 UART 2선과 GND만 연결합니다. 921600bps, 8N1입니다.

| H562 (코어 J4 헤더) | 방향 | ESP32-S3 |
| --- | --- | --- |
| PC6 = USART6 TX (J4-21) | → | GPIO48 (RX) |
| PC7 = USART6 RX (J4-23) | ← | GPIO45 (TX) |
| GND | — | GND |

**ESP의 GPIO45는 부트 스트래핑 핀**이라 H562의 TX(평소 High)를 연결하면 ESP가 켜질 때 Flash 전압이 1.8V로 잡혀 부팅하지 못할 수 있습니다. 그래서 GPIO45를 ESP의 출력(TX)으로 씁니다. 기존 bring-up 스케치(Serial2 RX=45, TX=48)와 방향이 반대이므로 배선할 때 확인합니다. 한쪽만 켜졌을 때 상대 핀으로 전류가 흐르지 않도록 각 신호선에 1kΩ 직렬 저항을 권장합니다.

### Network 메뉴

| 하위 메뉴 | 동작 |
| --- | --- |
| **Connect → Wi-Fi** | 주변 Wi-Fi를 검색해 신호가 강한 순서로 표시(막대 4단계). `*`는 비밀번호가 저장된 네트워크, `o`는 암호 없는 네트워크. 마지막 줄 `(Scan again)`으로 다시 검색 |
| **Connect → LTE** | APN 선택(Global `mobile`, SKT, SKT IoT, KT, KT IoT, LG U+, LG U+ IoT) 후 연결. 모뎀 부팅·망 등록에 수 분이 걸릴 수 있음 |
| **Connect → Off** | Wi-Fi를 끊고 LTE 모뎀 전원을 끔 |
| **Install** | Hub 목록을 받아 칩별로 표시 → 칩 → 프로젝트 → 확인 화면에서 SEL을 놓았다가 1초 눌러 설치 |
| **Status** | 연결 방식·상태, SSID 또는 APN, IP, 신호 세기, ESP 펌웨어 버전 |

Network 화면 아래쪽 상태 표시: `No ESP`(ESP 응답 없음 — 배선·전원·ESP 펌웨어 확인), `ESP ready`(ESP는 정상, 네트워크 미연결), `Wi-Fi...` / `LTE...`(연결 중), `Wi-Fi on` / `LTE on`(연결됨), `Wi-Fi fail` / `LTE fail`(연결 실패). Status 화면에서는 미연결을 `Not connected`로 표시합니다.

### 비밀번호 입력

버튼 4개로 입력하는 문자 바퀴입니다.

| 버튼 | 동작 |
| --- | --- |
| UP / DN | 가운데 큰 문자를 이전/다음으로. 누르고 있으면 빠르게 넘어감 |
| SEL (짧게) | 가운데 문자를 추가 |
| SEL (0.6초 이상) | 메뉴: `Done`(연결), `abc` / `ABC` / `123` / `#+=`(문자 묶음으로 이동), `Show text`, `Clear`, `Cancel` |
| M | 마지막 문자 삭제. 비어 있으면 목록으로 돌아감 |

입력 중에는 마지막 문자만 보이고 나머지는 `*`로 가려집니다. 메뉴의 `Show text`로 전체를 볼 수 있습니다. 바퀴 순서는 소문자 → 대문자 → 숫자 → 기호 → 공백(빈 상자 모양)입니다.

### 저장되는 정보

연결에 **한 번 성공한** 뒤에만 SD카드의 `/net/net.cfg`에 저장합니다. 최근 Wi-Fi 8개(SSID·비밀번호), 마지막 연결 방식, LTE APN이 들어갑니다. 파일은 H562 고유 ID로 만든 키로 뒤섞어 저장하고 CRC로 검사합니다. 다른 프로그래머에 꽂으면 빈 설정으로 읽힙니다. 이는 카드를 열었을 때 평문이 보이지 않게 하는 정도이며 강한 암호화는 아닙니다.

- 저장된 네트워크를 다시 고르면 비밀번호가 미리 채워진 입력 화면이 열립니다. 바뀐 비밀번호는 여기서 고칩니다.
- 전원을 켜면 ESP가 응답하는 대로 마지막으로 연결한 네트워크에 다시 연결합니다. Off로 끄면 다시 연결하지 않습니다.
- **SD Delete는 `/net` 폴더도 지웁니다.**

### Hub에서 설치

1. **Network → Install**을 열면 Hub의 `manifest.json`을 받아 프로젝트를 Hub가 정한 칩별로 묶어 보여 줍니다. 이 펌웨어가 기록할 수 없는 칩의 프로젝트는 표시하지 않습니다.
2. 프로젝트 오른쪽 `=`는 같은 버전이 SD에 있음, `^`는 다른 버전이 있음(업데이트)입니다.
3. 확인 화면에서 이름, Hub 최신 버전, `new` / `update` / `same`을 확인하고 SEL을 놓았다가 1초 누릅니다.
4. `fw/<id>/latest.txt`의 파일을 적힌 순서대로 받습니다. 파일마다 `.part`로 받은 뒤 크기와 CRC32가 맞을 때만 이름을 바꿉니다. 시작할 때 기존 `fw.txt`를 먼저 지우고 `fw.txt`를 마지막에 쓰므로, 중간에 실패하거나 M으로 멈추면 그 프로젝트는 목록에 보이지 않습니다. 다시 설치하면 됩니다.

설치를 시작하지 않는 경우와 화면 표시:

| 표시 | 이유 |
| --- | --- |
| `No ST loader!` | H723 프로젝트인데 SD에 `/prog/loaders/st/0x483.stldr`이 없음. 로더는 라이선스 때문에 Hub에 없으므로 PC에서 복사 |
| `Not connected` | Wi-Fi/LTE 연결 안 됨 |
| `SD full (16)` | SD에 이미 16개가 있고 새 프로젝트임 |
| `No SD card` | SD카드 없음 |

## 메뉴와 버튼

전원을 켜면 **Auto 대기 화면**이 표시되고, M을 누르면 아래 메뉴가 열립니다. 메뉴에서 M을 누르면 대기 화면으로 돌아갑니다.

| 메뉴 | 하위 메뉴 | 동작 |
| --- | --- | --- |
| **Auto setup** | | 칩별로 Auto에 사용할 프로젝트 지정 |
| **Manual** | **Flash** | 칩 선택 → 그 칩의 프로젝트 선택 → 필요한 섹터 삭제 → BIN 기록·검증 → 타깃 재시작 |
| | **Erase** | H723 / ESP32 / ESP32-S3 / ESP32-C3 선택 → 전체 Flash 삭제·빈 영역 검증 |
| **Storage** | **Installed bin** | SD카드의 프로젝트를 칩별로 표시. SEL로 이름·ID·크기·BIN 수·버전을 확인 |
| | **SD Delete** | SD카드의 파일과 폴더를 모두 삭제 |
| **Network** | **Connect** | Wi-Fi(검색 → 선택 → 비밀번호) / LTE(APN) / Off |
| | **Install** | Hub 프로젝트를 칩별로 골라 SD에 설치 |
| | **Status** | 연결 상태, IP, 신호 세기 |
| **Power** | | 3.3V out / 5V out ON·OFF, Supply(Job / Always). [타깃 전원 공급](#타깃-전원-공급-3v_out--5v_out) 참고 |
| **Info** | | `made by Higenis` 표시 |

**Installed bin**의 칩 목록 숫자는 기록할 수 없는 프로젝트까지 포함한 개수입니다. 프로젝트 목록 오른쪽의 `!`는 기록할 수 없는 프로젝트(BIN 누락, 크기 불일치, `fw.txt` 오류)이며, 상세 화면 셋째 줄에 이유가 표시됩니다. `*`는 Auto에 지정된 프로젝트입니다(상세 화면에서는 제목 옆). 지원하지 않는 `device=`의 프로젝트는 `Unknown`으로 묶입니다. 내용 검사(CRC)는 기록할 때 합니다.

**SD Delete**는 포맷이 아니라 **파일 삭제**입니다. FAT32 파일 시스템은 그대로 두고, 루트 아래의 모든 파일과 폴더(`prog/fw`, `auto*.cfg`, **ST 로더 `0x483.stldr`**, PC가 만든 폴더 포함)를 지웁니다. 확인 화면에서 SEL을 놓았다가 1초 눌러 시작하며, 먼저 항목 수를 센 뒤 삭제 진행률을 표시합니다. M으로 중단하면 이미 지운 항목은 복구되지 않습니다. 읽기 전용 파일도 지우며, 루트에서 8단계보다 깊은 폴더가 있으면 아무것도 지우지 않고 `Folder too deep`으로 멈춥니다. 삭제 후 H723 기록·삭제에는 로더를 다시 복사해야 합니다.

Erase도 대상 선택 후 확인 화면에서 SEL을 놓았다가 1초 눌러 시작합니다. H723은 SD카드의 로더가 필요하며 BIN은 없어도 됩니다. H723 완료 화면은 **`Erase / Complete`, `Blank OK`, `HALTED`**입니다. ESP는 SD 없이 전체 삭제하며 완료 후 **`STUB`**로 남습니다.

| 버튼 | 동작 |
| --- | --- |
| UP / DN | 메뉴·칩·프로젝트 선택 |
| SEL | 선택 / 수동 Flash·Erase·SD Delete·Install은 확인 화면에서 1초 누르기 / Auto 대기·결과 화면에서는 한 번 누르면 기록 / 비밀번호 입력에서는 문자 추가, 길게 누르면 메뉴 |
| M | 대기 화면에서 메뉴 열기 / 이전 화면 / 작업 중 취소 요청 |

취소는 현재 로더 작업이 끝난 뒤 처리됩니다. 이미 지우거나 기록한 내용은 되돌아가지 않습니다.

## CLI로 상태 확인

OLED만으로 기록할 수 있으며, 상세 로그가 필요하면 ST-Link VCP 또는 USB-UART를 연결합니다.

| H562 핀 | UART 어댑터 연결 |
| --- | --- |
| **PB15 — TX** | RX |
| **PB14 — RX** | TX |
| GND | GND |

현재 USART1은 **TX/RX SWAP 사용** 상태입니다. COM 포트 터미널을 **115200 / 8N1 / 흐름 제어 없음**으로 열면 부팅 로그와 `cli#` 프롬프트를 볼 수 있습니다.

```text
cli# prog list           SD카드 프로젝트 목록 (Hub 칩 ID, 프로젝트 ID, 버전, 이름, 기록 불가 사유)
cli# prog check h723     h723의 BIN·로더·CRC 검사
cli# swd probe           연결된 타깃 식별
cli# net status          ESP 연결 상태 (net hello | scan | wifi <ssid> [pw] | lte <apn> | off | get <url>)
cli# uart                UART1(디버그)·UART2(ESP 링크) 상태와 송수신 바이트 수
cli# power               타깃 전원 설정과 현재 출력 (power 3v3|5v on|off, power mode job|always)
```

`prog check`는 SD 파일만 검사하며 타깃 Flash를 변경하지 않습니다. CLI 출력은 시리얼 터미널에서 확인합니다.

## 주요 핀맵

| 기능 | H562 핀 / 설정 |
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
| 타깃 전원 3V_OUT / 5V_OUT | PB5 / PA9, HIGH에서 출력, 게이트 풀다운으로 기본 OFF |
| ESP 모드: CLK / SWO 단자 | PB4 = UART7 TX (AF11) / PC1 = 소프트웨어 RX (TIM2 + EXTI1) |
| ESP 모드: nRESET / DIO 단자 | PB3 = EN / PB1 = BOOT, 오픈 드레인 |
| H562 자체 SWDIO / SWCLK | PA13 / PA14 |
| USART1 TX / RX | PB15 / PB14, SWAP 사용 |
| USART6 TX / RX (ESP 네트워크 모듈) | PC6 / PC7, AF7, 921600bps, RX는 GPDMA1 채널 1 |
| 코어 W25Q256JV NCS / CLK | PB10 / PA3, OCTOSPI1 |
| 코어 W25Q256JV IO0 / IO1 | PC3 / PB0 |
| 코어 W25Q256JV IO2 / IO3 | PC2 / PA1, 현재 단일 데이터선 모드에서 HIGH |
| HSE / LSE | 25MHz / 32.768kHz |

## 문제가 있을 때

| 증상 | 확인할 내용 |
| --- | --- |
| `No SD card` | SD카드 삽입 및 감지 상태 |
| `SD not ready` | FAT32 포맷, 카드 접촉 상태 |
| `No firmware` | `/prog/fw/<프로젝트 ID>/fw.txt` 경로와 프로젝트 구성. Storage → Installed bin에서 `!` 표시와 사유 확인 |
| CRC 검사 실패 | BIN과 `fw.txt`가 같은 패키지인지 확인하고 준비 스크립트로 재생성 |
| 다운로드 후 화면·CLI가 멈춤 | 디버거가 `main()`에서 멈췄다면 F5로 계속 실행 |
| CLI가 나오지 않음 | COM 포트, 115200 설정, PB15 TX → 어댑터 RX 연결 |
| `Error_Handler`에서 정지 | 호출 스택과 `bsp_init_stage`, `bsp_init_status` 확인. 발진기 단계라면 HSE/LSE 점검 |
| `Set Auto project` | Auto에서 감지된 칩에 사용할 프로젝트 지정 |
| `Auto chip mismatch` | 지정된 프로젝트의 `device`와 연결한 칩 확인 |
| `Unsupported SWD` | 현재 H723 지원 범위인지, SWD 접촉·전원·잠금 상태 확인 |
| 타깃 연결·기록 실패 | H723 전원, 공통 GND, CLK/DIO 연결 확인 후 CLI의 실패 단계 확인 |

현재 펌웨어는 HSE 25MHz와 LSE 32.768kHz를 사용합니다. STM32용 SWD 로더는 H723 내부 Flash 전용입니다. ESP는 위 UART 절차를 사용하며, 그 밖의 타깃에는 별도 지원이 필요합니다.
