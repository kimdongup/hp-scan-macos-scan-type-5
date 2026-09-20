# Roadmap

## v0.2 — Flatbed SOAPHT baseline

- [x] macOS arm64 HPAIO backend
- [x] USB discovery
- [x] HPMUD SOAPHT channel
- [x] GetScannerElements / CreateScanJob / RetrieveImage / CancelJob
- [x] HTTP chunk decoding
- [x] DIME parsing
- [x] JPEG reconstruction
- [x] Gray 150/300/600
- [x] Color 300/600
- [x] clean build automation
- [x] native Mach-O SOAPHT plugin

## v0.3 — ADF

- [ ] expose ADF source
- [ ] paper detection
- [ ] ADF CreateScanJob
- [ ] multi-page lifecycle
- [ ] 150/300 dpi validation

## v0.4 — Geometry and robustness

- [ ] partial scan geometry
- [ ] crop support
- [ ] option validation
- [ ] timeout/retry and cancel recovery
- [ ] malformed HTTP/DIME handling

## v0.5 — Wider compatibility

- [ ] identify additional HPLIP scan-type=5 devices
- [ ] test capability differences
- [ ] compatibility table

## v1.0 — macOS integration

- [ ] polished installer/uninstaller
- [ ] regression procedure
- [ ] release artifacts
- [ ] Image Capture / AirScan validation
- [ ] fresh-Mac release test
