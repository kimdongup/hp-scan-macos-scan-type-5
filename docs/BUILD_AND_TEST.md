# Build and Test Guide

## Clean build

```bash
cd ~/hp-scan-macos
rm -rf /tmp/hplip-build
./build.sh
```

## Verify runtime config

```bash
strings /opt/homebrew/lib/libhpmud.0.dylib | grep hplip.conf
```

Expected:

```text
/opt/homebrew/etc/hp/hplip.conf
```

## Verify plugin

```bash
file /opt/homebrew/share/hplip/scan/plugins/bb_soapht.so
```

Expected arm64 Mach-O dylib.

## Verify scanner

```bash
scanimage -L
```

## Regression matrix

```text
Gray 150
Gray 300
Gray 600
Color 300
Color 600
```

Example:

```bash
scanimage   -d 'hpaio:/usb/HP_LaserJet_Pro_MFP_M127fn?serial=YOUR_SERIAL'   --resolution 300 --mode Color --source Flatbed --format=jpeg   > /tmp/m127-color-300.jpg
```

Representative dimensions:

```text
150 dpi ≈ 1274 × 1753
300 dpi ≈ 2549 × 3506
600 dpi ≈ 5099 × 7013
```

## Plugin-only rebuild

```bash
HPLIP_SRC='/tmp/hplip-build/hplip-3.25.8'   ./soapht-macos/build-plugin.sh
```

Install:

```bash
sudo install -m 0755 soapht-macos/bb_soapht.so   /opt/homebrew/share/hplip/scan/plugins/bb_soapht.so
```
