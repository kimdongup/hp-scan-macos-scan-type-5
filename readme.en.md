# hp-scan-macos-scan-type-5

A macOS / Apple Silicon port that extends the HPLIP SANE backend with support for **HP `scan-type=5` SOAPHT scanners**, currently focused on the **HP LaserJet Pro MFP M127fn** over USB.

This repository is based on [`nricaurte/hp-scan-macos`](https://github.com/nricaurte/hp-scan-macos). The upstream project primarily targets LEDM (`scan-type=7`) devices; this fork adds a clean-room native macOS SOAPHT compatibility plugin.

## Verified

| Component | Status |
|---|---|
| macOS 26 Tahoe | ✅ |
| Apple Silicon / arm64 | ✅ |
| HPLIP 3.25.8 | ✅ |
| Homebrew SANE 1.4.x | ✅ |
| HP LaserJet Pro MFP M127fn via USB | ✅ |
| HPLIP `scan-type=5` | ✅ |
| Flatbed Gray 150/300/600 dpi | ✅ |
| Flatbed Color 300/600 dpi | ✅ |
| ADF | Planned |

## Protocol flow

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

SOAPHT uses the HPMUD channel:

```text
HP-SOAP-SCAN
```

## Install

```bash
git clone https://github.com/kimdongup/hp-scan-macos-scan-type-5.git
cd hp-scan-macos-scan-type-5
./build.sh
```

## Example scan

```bash
scanimage   -d 'hpaio:/usb/HP_LaserJet_Pro_MFP_M127fn?serial=YOUR_SERIAL'   --resolution 300   --mode Color   --source Flatbed   --format=jpeg   > ~/Desktop/m127-color-300.jpg
```

## Native SOAPHT plugin

```text
soapht-macos/bb_soapht_macos.c
→ soapht-macos/bb_soapht.so
→ /opt/homebrew/share/hplip/scan/plugins/bb_soapht.so
```

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Build & Test](docs/BUILD_AND_TEST.md)
- [Research History](docs/RESEARCH_HISTORY.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Roadmap](docs/ROADMAP.md)
- [Development Prompt](prompt.en.md)

## Upstream

```text
origin   → kimdongup/hp-scan-macos-scan-type-5
upstream → nricaurte/hp-scan-macos
```

## Status

Flatbed SOAPHT scanning is working on the tested M127fn. The next major target is ADF support, followed by macOS-native scanning integration.
