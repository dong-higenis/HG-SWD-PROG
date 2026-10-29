# ESP32S3_NET — H5용 네트워크 코프로세서 펌웨어

HG-SWD-PROG(H5)가 UART로 부리는 ESP32-S3 펌웨어입니다. ESP는 **Wi-Fi 또는 LTE 연결 + HTTPS GET 스트리밍**만 하는 범용 통로이고, Hub 형식 해석과 SD 기록은 H5가 합니다. 프로토콜은 [PROTOCOL.md](PROTOCOL.md)(v1)를 그대로 구현합니다.

H5에서 ESP를 다시 기록할 수 없으므로(TX/RX/GND만 연결) ESP 펌웨어는 USB로 한 번 올려 두고 오래 쓰는 것을 전제로 합니다.

## 배선

H5 링크 (PROTOCOL.md와 동일, 921600bps 8N1, 흐름 제어 없음):

| H5 | 방향 | ESP32-S3 |
| --- | --- | --- |
| PC6 USART6_TX (AF7) | → | GPIO48 (Serial2 RX) |
| PC7 USART6_RX (AF7) | ← | GPIO45 (Serial2 TX) |
| GND | — | GND |

- GPIO45는 부트 스트래핑 핀(VDD_SPI 전압 선택)이라 ESP의 **출력(TX)**으로만 씁니다. H5 TX(평소 High)를 GPIO45에 물리면 ESP가 부팅하지 못할 수 있습니다. 보드 bring-up 스케치(HG-ESP32S3-LTE)와 RX/TX가 반대인 것은 의도된 것입니다.
- 각 신호선에 1kΩ 직렬 저항을 권장합니다.

보드 내부(기존 보드 그대로):

| 기능 | ESP32-S3 | 비고 |
| --- | --- | --- |
| LTE 모뎀 UART (UART1, 115200) | TX GPIO17 → 모뎀 RX, RX GPIO18 ← 모뎀 TX | 코어 PPP 라이브러리(esp_modem)가 사용 |
| LTE 모뎀 전원 | GPIO5 | HIGH = 켜짐. 부팅 시 LOW(꺼짐) |
| 상태 LED (WS2812) | GPIO38, GPIO39 | 두 LED가 같은 색 |
| 디버그 콘솔 | USB (`Serial`, HW CDC) | 115200 |

## 빌드 / 업로드

arduino-cli, 코어 `esp32:esp32` 3.3.11, 라이브러리 Adafruit NeoPixel(설치되어 있음).

USB 콘솔을 쓰려면 **USB CDC On Boot**를 켠 FQBN으로 빌드해야 합니다. `sketch.yaml`에 기본 FQBN(`esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc`)이 들어 있어서 `--fqbn` 없이 실행하면 그 설정이 쓰입니다.

```sh
CLI="/c/Program Files/Arduino CLI/arduino-cli.exe"
"$CLI" compile --build-path /tmp/esp-net-build firmware/ESP32S3_NET
"$CLI" upload  --build-path /tmp/esp-net-build -p COM5 firmware/ESP32S3_NET
"$CLI" monitor -p COM5 -c baudrate=115200
```

`--fqbn esp32:esp32:esp32s3`(기본값, CDC 꺼짐)로 빌드해도 동작은 같지만 디버그 콘솔이 USB가 아니라 UART0(GPIO43/44)로 나갑니다. 빌드 결과물은 저장소 밖(`--build-path`)에 두세요.

## 파일 구성

| 파일 | 내용 |
| --- | --- |
| `ESP32S3_NET.ino` | setup/loop. loop가 H5 링크, Wi-Fi 상태 머신, CLI, LED를 돌림 |
| `config.h` | 핀, 보드레이트, 시간 제한 등 상수 |
| `protocol_defs.h`, `crc16.h` | 프로토콜 상수, CRC-16/CCITT-FALSE(컴파일 타임 검증) |
| `link.*` | 프레임 파서(바이트 간 50ms 타임아웃), CRC, 재전송 캐시 |
| `protocol.*` | 명령 0x01~0x32 처리 |
| `net.*` | 모드 배타 제어(Wi-Fi ↔ LTE), STATUS 구성 |
| `net_wifi.*` | 비동기 스캔, 접속 상태 머신 |
| `net_lte.*` | LTE(PPP) 상태 머신. 모뎀 호출이 블로킹이라 별도 FreeRTOS 태스크에서 실행 |
| `net_http.*` | HTTPS GET 클라이언트(직접 구현, chunked 디코딩) |
| `cli.*`, `status_led.*`, `log.h` | USB 콘솔, LED, 로그 |
| `ca_bundle.h` | 루트 인증서 묶음(생성 파일) |
| `tools/make_ca_bundle.py`, `tools/ca_roots.pem` | 인증서 묶음 생성기와 원본 루트 |
| `tools/net_link_test.py` | H5 역할을 하는 PC용 벤치 도구 |

## 동작 요약

- **모드 배타**: `WIFI_SCAN`/`WIFI_JOIN`은 LTE를 끄고(모뎀 전원 즉시 차단), `LTE_JOIN`은 Wi-Fi를 끄고, `OFF`는 둘 다 끕니다. 모드를 바꾸는 명령은 열린 HTTP 연결도 닫습니다. 부팅 직후는 꺼짐 상태입니다.
- **Wi-Fi**: 스캔은 비동기(`WIFI_LIST`가 끝날 때까지 BUSY). RSSI 내림차순, 같은 SSID는 가장 강한 것만, 숨은 SSID 제외, 최대 16개. 접속은 20초 제한이며 실패 사유는 `wrong password`/`no AP`/`assoc failed`/`timeout`. 자격 증명은 NVS에 저장하지 않습니다. 접속 중에 `WIFI_SCAN`을 보내면 BUSY입니다.
- **LTE**: GPIO5로 모뎀 전원 → 8초 부팅 대기(`LTE_BOOT_MS`) → `PPP.begin(PPP_MODEM_GENERIC)`(3초마다 재시도) → SIM 확인(`AT+CPIN?`) → 망 등록(`AT+CGATT?`) → PPP 데이터 모드 → IP. 전체 180초 제한. 실패 사유는 `modem no response`/`no SIM`/`SIM locked`/`not attached`/`dial failed`/`no IP`/`link lost`. 실패하면 모뎀 전원을 끕니다. STATUS detail은 연결 중에는 APN, 연결되면 `사업자/APN`. RSSI는 `AT+CSQ`를 dBm으로 바꾼 값이며 데이터 모드에 들어간 뒤에는 접속 직전 값으로 고정됩니다(`LTE_USE_CMUX 1`이면 15초마다 갱신, 미검증).
- **HTTPS**: `https://`만, 리다이렉트를 따르지 않음(3xx는 code로 그대로). 모든 HTTP 상태 코드(404 포함)는 상태 OK + `code`로 돌려주고, 상태 `HTTP`(5)는 DNS/TCP/TLS 실패나 응답 형식 오류에만 씁니다. 요청은 `HTTP/1.1`, `Connection: close`, `Accept-Encoding: identity`. `Content-Length`가 있으면 그 길이, chunked이면 디코딩해서 본문만 주고 length는 0xFFFFFFFF, 둘 다 없으면 연결이 닫힐 때까지 읽습니다. 본문 길이보다 먼저 연결이 끊기면 `HTTP_READ`가 상태 `HTTP` + `closed early`를 돌려줍니다.
- **링크**: 같은 `cmd`·`seq`가 다시 오면 다시 실행하지 않고 직전 응답 프레임을 그대로 보냅니다. CRC가 틀리거나 `len` > 1100이면 응답 없이 버립니다. payload가 정의되지 않은 명령(HELLO, STATUS 등)은 붙어 온 payload를 무시하고, payload가 있는 명령은 길이를 엄격히 검사합니다(BAD_REQUEST).
- `HTTP_GET`/`HTTP_READ`는 loop 안에서 각각 최대 약 20초/5초 블로킹합니다(H5 대기 25초/8초 안). LTE 접속 과정은 별도 태스크라 STATUS 폴링(300ms)에 항상 바로 응답합니다.

## USB CLI (115200)

| 명령 | 동작 |
| --- | --- |
| `help` | 명령 목록 |
| `status` | 모드/상태/RSSI/IP/detail, 링크 통계(프레임, 중복, CRC 오류, 타임아웃), 힙 |
| `scan` | Wi-Fi 스캔, 끝나면 목록 출력 |
| `wifi <ssid> [pw]` | Wi-Fi 접속. 공백 있는 SSID는 `"..."` |
| `lte <apn>` | LTE 시작. APN 생략(또는 `""`)은 망 기본값. 예: `lte lte.sktelecom.com`, `lte internet.lguplus.co.kr`, `lte lte.ktfwing.com` |
| `off` | Wi-Fi 끄기, 모뎀 전원 끄기 |
| `get <https-url>` | HTTPS GET: code, length, 본문 크기, CRC32, 속도, 앞 64바이트 hex |
| `log on` / `log off` | 프레임·HTTP 상세 로그(기본 꺼짐) |

상태 변화와 오류는 항상 로그에 찍히고, 프레임 단위 로그는 `log on`일 때만 나옵니다. USB 터미널을 읽지 않을 때 로그 때문에 H5 링크가 멈추지 않도록 USB 출력은 버퍼가 차면 버립니다. CLI의 `get`은 H5와 같은 HTTP 세션을 쓰므로 H5가 다운로드 중일 때는 쓰지 마세요.

## LED

| 색 | 의미 |
| --- | --- |
| 꺼짐 | 네트워크 꺼짐 / 대기 |
| 파랑 | 연결 중 또는 Wi-Fi 스캔 중 |
| 초록 | 연결됨 |
| 빨강 | 연결 실패 |

색 순서는 bring-up 스케치를 따라 `NEO_BRG`입니다(`config.h`의 `LED_COLOR_ORDER`). 파랑이 다른 색으로 보이면 `NEO_GRB`로 바꾸세요.

## 루트 인증서 묶음 재생성

`ca_bundle.h`는 생성 파일입니다. 포함 루트: ISRG Root X1/X2, DigiCert Global Root CA/G2, USERTrust RSA/ECC. 현재 `can-dubu.github.io`는 `*.github.io ← Let's Encrypt YR1 ← ISRG Root YR ← ISRG Root X1`(교차 서명) 체인을 보내며, 이 6개 루트만으로 검증되는 것을 `openssl s_client -CAfile tools/ca_roots.pem -no-CApath -no-CAstore`로 확인했습니다.

```sh
cd firmware/ESP32S3_NET
python tools/make_ca_bundle.py                       # Git for Windows의 ca-bundle.crt에서 루트 추출
python tools/make_ca_bundle.py --pem tools/ca_roots.pem   # 저장된 루트로 다시 생성
python tools/make_ca_bundle.py --ca-file /path/to/ca-bundle.crt
```

스크립트는 (1) `C:\Program Files\Git\mingw64\etc\ssl\certs\ca-bundle.crt`(Git Bash의 `/mingw64/etc/ssl/certs/ca-bundle.crt`)에서 위 루트를 골라 `tools/ca_roots.pem`에 저장하고, (2) 코어의 `tools/gen_crt_bundle.py`(`%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\3.3.11\tools`)로 묶음을 만든 뒤, (3) ESP-IDF 5.5 형식으로 변환해 `ca_bundle.h`를 씁니다. `cryptography` 패키지가 필요합니다. 루트를 추가하려면 스크립트의 `ROOTS` 목록을 고치세요.

변환이 필요한 이유: 코어 3.3.11의 `gen_crt_bundle.py`는 아직 옛 형식(BE 개수 + BE 길이)을 출력하지만, 코어에 링크된 ESP-IDF 5.5.5의 `esp_crt_bundle_set()`은 새 형식(LE u32 오프셋 표 + LE 길이)만 받습니다. 옛 형식을 넘기면 거부되고 코어 내장 전체 묶음으로 조용히 대체됩니다. 그래서 펌웨어는 부팅 때 묶음을 검사해 `CA bundle ok (6 roots)` 또는 `CA bundle REJECTED`를 로그에 남깁니다.

Let's Encrypt가 ISRG Root X1 교차 서명 없이 Root YR/YE 체인만 보내게 되면 해당 루트를 추가해야 합니다.

## 벤치 도구

USB-UART 어댑터를 H5 자리에 연결(어댑터 TX → GPIO48, 어댑터 RX ← GPIO45, GND)하고:

```sh
pip install pyserial
python tools/net_link_test.py COM7                         # HELLO, STATUS, 스캔
python tools/net_link_test.py COM7 --ssid MyAP --pw secret # + Wi-Fi 접속, manifest.json 다운로드
python tools/net_link_test.py COM7 --apn lte.sktelecom.com --no-scan
python tools/net_link_test.py --selftest                   # 하드웨어 없이 CRC/프레임 검사
```

HTTP 단계에서는 본문 크기와 CRC32를 출력하고, 첫 `HTTP_READ`를 같은 `seq`로 한 번 더 보내 재전송 캐시가 같은 응답을 돌려주는지도 확인합니다.

## 하드웨어에서 아직 확인하지 않은 것

이 펌웨어는 컴파일과 PC 쪽 자체 시험만 거쳤습니다. 실제 보드에서 확인할 항목:

1. H5 ↔ ESP 921600bps 링크(GPIO48/45), 부팅 시 GPIO45 스트래핑 문제가 없는지.
2. Wi-Fi 스캔/접속/실패 사유(특히 `wrong password` 판정), 다운로드 속도.
3. **LTE 전체**: Quectel 모뎀과 `PPP_MODEM_GENERIC`(esp_modem) 조합, GPIO5 전원 후 8초 부팅 대기가 충분한지, `PPP.begin()` 재시도, `AT+CGATT?` 등록 판정, `ATD*99#` 데이터 모드 진입, DNS. 안 되면 `net_lte.cpp`의 `LTE_MODEM_MODEL`을 `PPP_MODEM_BG96`으로 바꿔 보세요. CMUX(`LTE_USE_CMUX`)는 미검증입니다.
4. 실제 TLS 핸드셰이크(위 체인, 내장 묶음 검사 로그), chunked 응답 처리(현재 GitHub Pages는 Content-Length를 보냄).
5. LED 색 순서.
