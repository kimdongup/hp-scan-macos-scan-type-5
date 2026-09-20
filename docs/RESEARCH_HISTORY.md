# Research History

## Problem

The M127fn is HPLIP `scan-type=5`. Linux uses a SOAPHT binary plugin that cannot be loaded natively on macOS.

## Linux plugin findings

Exports eight `bb_*` scanner functions and depends primarily on the HPMUD channel API.

## Traffic tracing

Observed:

```text
HP-SOAP-SCAN
GetScannerElements
CreateScanJob
RetrieveImage
CancelJob
```

Capabilities include JFIF/HPRAW, grayscale/RGB modes, Flatbed and ADF, with platen optical resolution up to 1200 dpi and ADF up to 300 dpi.

## RetrieveImage

```text
HTTP 200
Content-Type: application/dime
Transfer-Encoding: chunked
```

After HTTP dechunking, the payload is a DIME stream. JPEG data is carried in `image/jfif` records and may span continuation records.

## Critical parser fix

Naive JPEG SOI→EOI extraction included DIME continuation headers and corrupted the image. The corrected parser appends only DIME data fields for `image/jfif` records.

## Build fixes

- HPLIP config compiled for `/opt/homebrew/etc/hp/hplip.conf`
- `libhpipp.la` removed from scanner-only dependencies because macOS `ar` rejects the empty archive

## Verified result

```text
Gray 150 / 300 / 600
Color 300 / 600
```
