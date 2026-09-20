# hp-scan-macos-scan-type-5

macOS 26 Tahoe / Apple Silicon에서 **HP LaserJet Pro MFP M127fn**의 USB 스캔 기능을 복원하기 위한 HPLIP/SANE 기반 프로젝트입니다.

이 저장소는 [`nricaurte/hp-scan-macos`](https://github.com/nricaurte/hp-scan-macos)를 기반으로 하며, 기존 프로젝트의 LEDM(`scan-type=7`) 지원에 더해 **SOAPHT 계열 `scan-type=5` 장치**를 macOS에서 사용할 수 있도록 확장합니다.

현재 핵심 목표는 Linux용 HP proprietary plugin에 의존하지 않고, macOS용 native Mach-O SOAPHT compatibility plugin을 통해 M127fn을 동작시키는 것입니다.

## 현재 확인된 환경

| 항목 | 상태 |
|---|---|
| macOS 26 Tahoe | ✅ |
| Apple Silicon / arm64 | ✅ |
| HPLIP 3.25.8 | ✅ |
| Homebrew SANE 1.4.x | ✅ |
| libusb | ✅ |
| HP LaserJet Pro MFP M127fn USB | ✅ |
| `scan-type=5` / SOAPHT | ✅ |
| Flatbed Gray 150 dpi | ✅ |
| Flatbed Gray 300 dpi | ✅ |
| Flatbed Gray 600 dpi | ✅ |
| Flatbed Color 300 dpi | ✅ |
| Flatbed Color 600 dpi | ✅ |
| ADF | 예정 |
| Apple Image Capture 직접 연동 | 후속 작업 |

테스트 장치:

```text
HP LaserJet Pro MFP M127fn
USB VID:PID 03f0:322a
HPLIP scan-type=5
Protocol: SOAPHT
```

## 왜 이 프로젝트가 필요한가

M127fn은 HPLIP에서 스캐너로 인식되지만, Linux 환경에서는 SOAPHT 스캔 처리를 위해 HP binary plugin을 사용합니다. 해당 Linux plugin은 macOS에서 직접 사용할 수 없습니다.

원본 plugin의 외부 의존성을 분석한 결과 핵심 통신은 HPLIP의 `hpmud` 채널 API를 통해 이루어지며, 주요 채널은 다음과 같습니다.

```text
HP-SOAP-SCAN
```

실제 장치 트래픽 분석을 통해 다음 흐름을 확인했습니다.

```text
GetScannerElements
        ↓
CreateScanJob
        ↓
RetrieveImage
        ↓
DIME records
        ↓
JPEG image
        ↓
CancelJob
```

이 프로젝트는 이 동작을 clean-room 방식으로 재구현한 macOS arm64 plugin을 포함합니다.

## 저장소 구조

```text
hp-scan-macos-scan-type-5/
├── README.md
├── readme.en.md
├── prompt.md
├── prompt.en.md
├── build.sh
├── LICENSE
├── soapht-macos/
│   ├── bb_soapht_macos.c
│   └── build-plugin.sh
├── patches/
│   ├── 01-darwin-headers.patch
│   ├── 02-musb-macos.patch
│   ├── 03-hpaio-uninit-fix.patch
│   └── 04-soapht-macos-plugin.patch
├── stubs/
├── bin/
├── app/
├── airscan-bridge/
└── docs/
    ├── ARCHITECTURE.md
    ├── BUILD_AND_TEST.md
    ├── RESEARCH_HISTORY.md
    ├── TROUBLESHOOTING.md
    └── ROADMAP.md
```

## 설치

```bash
git clone https://github.com/kimdongup/hp-scan-macos-scan-type-5.git
cd hp-scan-macos-scan-type-5
./build.sh
```

`build.sh`는 HPLIP 3.25.8 다운로드, Darwin patch 적용, `libhpmud`/`libhpip`/`libsane-hpaio` 빌드, native `bb_soapht.so` 빌드 및 설치, SANE backend 등록까지 수행합니다.

## 장치 확인

```bash
scanimage -L
```

정상 예:

```text
device `hpaio:/usb/HP_LaserJet_Pro_MFP_M127fn?serial=XXXXXXXXXXXX' is a Hewlett-Packard HP_LaserJet_Pro_MFP_M127fn all-in-one
```

## 지원 옵션 확인

```bash
scanimage -d 'hpaio:/usb/HP_LaserJet_Pro_MFP_M127fn?serial=YOUR_SERIAL' --all-options
```

현재 주요 옵션:

```text
--mode Gray|Color
--resolution 150|300|600dpi
--source Flatbed
--compression None|JPEG
```

## 실제 스캔

### 300 dpi Gray

```bash
scanimage   -d 'hpaio:/usb/HP_LaserJet_Pro_MFP_M127fn?serial=YOUR_SERIAL'   --resolution 300   --mode Gray   --source Flatbed   --format=jpeg   > ~/Desktop/m127-gray-300.jpg
```

### 300 dpi Color

```bash
scanimage   -d 'hpaio:/usb/HP_LaserJet_Pro_MFP_M127fn?serial=YOUR_SERIAL'   --resolution 300   --mode Color   --source Flatbed   --format=jpeg   > ~/Desktop/m127-color-300.jpg
```

## SOAPHT plugin

소스:

```text
soapht-macos/bb_soapht_macos.c
```

설치 위치:

```text
/opt/homebrew/share/hplip/scan/plugins/bb_soapht.so
```

plugin은 다음 HPLIP symbols를 runtime에 사용합니다.

```text
hpmud_open_channel
hpmud_read_channel
hpmud_write_channel
hpmud_close_channel
```

## HPLIP runtime configuration

```text
/opt/homebrew/etc/hp/hplip.conf
```

확인:

```bash
strings /opt/homebrew/lib/libhpmud.0.dylib | grep hplip.conf
```

## 문서

- [Architecture](docs/ARCHITECTURE.md)
- [Build & Test](docs/BUILD_AND_TEST.md)
- [Research History](docs/RESEARCH_HISTORY.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Roadmap](docs/ROADMAP.md)
- [Development Prompt / Korean](prompt.md)
- [Development Prompt / English](prompt.en.md)

## Upstream

```text
https://github.com/nricaurte/hp-scan-macos
```

권장 Git remote 구성:

```text
origin   → https://github.com/kimdongup/hp-scan-macos-scan-type-5.git
upstream → https://github.com/nricaurte/hp-scan-macos.git
```

## Project status

현재 **HP LaserJet Pro MFP M127fn USB Flatbed SOAPHT scanning on macOS Tahoe / Apple Silicon**은 동작 검증되었습니다.

다음 개발 대상은 ADF, geometry/crop, regression tests, native macOS scanning integration입니다.

## License

원본 HPLIP 및 upstream 프로젝트의 라이선스를 따릅니다. HPLIP-derived patch 및 integration code를 배포할 때에는 upstream license 조건을 확인하십시오.
