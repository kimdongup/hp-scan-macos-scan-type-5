# Architecture

## Overview

```text
scanimage / SANE frontend
        │
        ▼
libsane-hpaio
        │
        ▼
HPLIP soapht backend
        │
        ▼
bb_soapht.so
        │
        ▼
libhpmud
        │
        ▼
USB
        │
        ▼
HP LaserJet Pro MFP M127fn
```

## Native plugin API

```text
bb_open
bb_close
bb_get_parameters
bb_is_paper_in_adf
bb_start_scan
bb_get_image_data
bb_end_page
bb_end_scan
```

## HPMUD API

```text
hpmud_open_channel
hpmud_write_channel
hpmud_read_channel
hpmud_close_channel
```

SOAPHT channel:

```text
HP-SOAP-SCAN
```

## Lifecycle

```text
bb_open → GetScannerElements
bb_start_scan → CreateScanJob
bb_get_image_data → RetrieveImage → HTTP chunking → DIME → JPEG
bb_end_scan → CancelJob
```

## Current scope

Implemented: USB, Flatbed, Gray, RGB Color, 150/300/600 dpi, JPEG/JFIF.

Planned: ADF, partial geometry, wider scan-type=5 compatibility, native macOS integration.
