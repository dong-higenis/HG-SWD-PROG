# H5 ↔ ESP32-S3 네트워크 링크 프로토콜 (v1)

H5(HG-SWD-PROG)가 마스터, ESP32-S3(이 폴더의 `ESP32S3_NET` 스케치)가 슬레이브입니다. ESP는 **Wi-Fi/LTE 연결과 HTTPS GET만** 담당하는 범용 통로입니다. Hub 형식(`manifest.json`, `latest.txt`)의 해석과 SD 기록은 모두 H5가 합니다. ESP는 H5에서 다시 기록할 수 없으므로(UART 2선만 연결) 이 프로토콜은 Hub가 바뀌어도 유지되게 일반적으로 정의합니다.

## 물리 계층

| H5 | 방향 | ESP32-S3 |
| --- | --- | --- |
| PC6 USART6_TX (AF7) | → | GPIO48 (Serial2 RX) |
| PC7 USART6_RX (AF7) | ← | GPIO45 (Serial2 TX) |
| GND | — | GND |

- 921600bps, 8N1, 흐름 제어 없음. 전원은 각자 공급합니다.
- **ESP의 GPIO45는 부트 스트래핑 핀(VDD_SPI 전압 선택)입니다.** H5의 TX는 평소 High이므로 GPIO45에 연결하면 ESP가 켜질 때 Flash 전압이 1.8V로 잡혀 부팅하지 못할 수 있습니다. 그래서 GPIO45는 ESP의 **출력(TX)**, GPIO48을 입력(RX)으로 씁니다.
- 한쪽만 켜진 상태에서 상대 핀으로 전류가 흐르지 않도록 각 신호선에 1kΩ 직렬 저항을 권장합니다.

## 프레임

```
+------+------+-----+-----+--------+--------+-------------+--------+--------+
| 0xA5 | 0x5A | cmd | seq | len_lo | len_hi | payload[len] | crc_lo | crc_hi |
+------+------+-----+-----+--------+--------+-------------+--------+--------+
```

- `len` ≤ 1100. CRC는 CRC-16/CCITT-FALSE(다항식 0x1021, 초기값 0xFFFF, 반사 없음, 최종 XOR 없음)이며 `cmd`부터 payload 끝까지 계산합니다. 정수는 모두 little-endian입니다.
- 요청(H5→ESP)의 `cmd`는 0x00~0x7F, 응답(ESP→H5)은 `cmd | 0x80`이고 `seq`는 요청과 같습니다.
- 응답 payload의 첫 바이트는 상태 코드입니다.

| 상태 | 값 | 뜻 |
| --- | --- | --- |
| OK | 0 | 성공 |
| BUSY | 1 | 진행 중(스캔·연결 등). 나중에 다시 묻습니다 |
| ERROR | 2 | 실패. 뒤에 ASCII 사유가 올 수 있음 |
| BAD_REQUEST | 3 | 길이·형식 오류 |
| NOT_CONNECTED | 4 | 네트워크 미연결 |
| HTTP | 5 | HTTP 연결·TLS·상태 오류. 뒤에 ASCII 사유 |
| TIMEOUT | 6 | ESP 쪽 대기 시간 초과 |
| UNKNOWN_CMD | 7 | 모르는 명령 |

## 재전송

- H5는 새 요청마다 `seq`를 1 늘립니다. 응답이 없거나 CRC가 틀리면 **같은 `seq`로 같은 요청**을 다시 보냅니다(최대 3번).
- ESP는 마지막으로 처리한 요청의 `cmd`·`seq`와 응답을 보관합니다. 같은 `cmd`·`seq`가 다시 오면 **다시 실행하지 않고** 보관한 응답을 그대로 보냅니다. 그래서 `HTTP_READ`처럼 되돌릴 수 없는 명령도 안전하게 재시도할 수 있습니다.
- CRC가 틀린 요청은 버리고 응답하지 않습니다.

## 명령

| cmd | 이름 | 요청 payload | 응답 payload (상태 뒤) | 비고 |
| --- | --- | --- | --- | --- |
| 0x01 | HELLO | 없음 | `proto(1)=1`, ESP 펌웨어 버전 ASCII | 연결 확인 |
| 0x02 | STATUS | 없음 | `mode(1)`, `state(1)`, `rssi(int8)`, `ip(4)`, `detail` ASCII | 아래 표 |
| 0x03 | OFF | 없음 | — | Wi-Fi 끊기, LTE 모뎀 전원 끄기 |
| 0x10 | WIFI_SCAN | 없음 | — | 비동기 스캔 시작. 다른 모드가 켜져 있으면 끄고 시작 |
| 0x11 | WIFI_LIST | 없음 | `count(1)`, 반복 `{rssi(int8), auth(1), ssid_len(1), ssid}` | 스캔 중이면 BUSY |
| 0x12 | WIFI_JOIN | `ssid_len(1)`, ssid(≤32), `pw_len(1)`, pw(≤64) | — | 비동기. LTE가 켜져 있으면 먼저 끔 |
| 0x20 | LTE_JOIN | `apn_len(1)`, apn(≤63) | — | 비동기. Wi-Fi가 켜져 있으면 먼저 끔 |
| 0x30 | HTTP_GET | `url_len(2)`, url(≤255, `https://`) | `code(2)`, `length(4)` | 응답 헤더까지 받고 반환(최대 20s). 길이를 모르면 0xFFFFFFFF |
| 0x31 | HTTP_READ | `max(2)` (1~1024) | 데이터 0~max 바이트 | OK에 데이터 0바이트면 본문 끝. 데이터가 없으면 최대 5s 대기 후 TIMEOUT |
| 0x32 | HTTP_CLOSE | 없음 | — | 연결 정리. 새 `HTTP_GET`도 이전 연결을 닫음 |

`WIFI_LIST`는 RSSI가 강한 순서로 최대 16개이며, 같은 SSID는 가장 강한 것만, 숨은(빈) SSID는 빼고 보냅니다. `auth` 0은 암호 없음, 1은 암호 있음입니다.

`STATUS` 값:

| 필드 | 값 |
| --- | --- |
| mode | 0 꺼짐, 1 Wi-Fi, 2 LTE |
| state | 0 대기, 1 연결 중, 2 연결됨, 3 실패 |
| rssi | Wi-Fi는 dBm, LTE는 `AT+CSQ`를 dBm으로 환산(모르면 0) |
| ip | IPv4, 연결 전에는 0.0.0.0 |
| detail | Wi-Fi: SSID / LTE: APN 또는 사업자 / 실패 시 사유 (≤ 47자) |

## 시간 제한 (H5 기준)

| 명령 | 응답 대기 |
| --- | --- |
| HTTP_GET | 25s |
| HTTP_READ | 8s |
| 그 밖 | 300ms |

## HTTPS

ESP는 서버 인증서를 검증합니다. 루트 인증서 묶음(ISRG Root X1/X2, DigiCert, Sectigo/USERTrust 등)을 펌웨어에 넣어 CA가 바뀌어도 동작하게 합니다. 리다이렉트는 따르지 않으며 3xx는 그대로 `code`로 돌려줍니다.
