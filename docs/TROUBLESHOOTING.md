# Troubleshooting

## No scanner in `scanimage -L`

```bash
grep '^hpaio$' /opt/homebrew/etc/sane.d/dll.conf
ioreg -p IOUSB -l -w 0 | grep -i -A20 'M127'
```

## Wrong HPLIP config path

```bash
strings /opt/homebrew/lib/libhpmud.0.dylib | grep hplip.conf
```

Correct:

```text
/opt/homebrew/etc/hp/hplip.conf
```

## `ar: no archive members specified`

Cause: empty `libhpipp.la` with network build disabled. Remove it from generated HPAIO dependencies before make.

## `clang: no such file or directory: bb_soapht_macos.c`

`build-plugin.sh` must compile `$SCRIPT_DIR/bb_soapht_macos.c`, not a path relative to the caller's current directory.

## Permission denied installing plugin

```bash
sudo install -m 0755 soapht-macos/bb_soapht.so   /opt/homebrew/share/hplip/scan/plugins/bb_soapht.so
```

## JPEG corruption

Do not copy bytes between JPEG SOI/EOI across the whole HTTP body. Parse DIME and concatenate only `image/jfif` record data.
