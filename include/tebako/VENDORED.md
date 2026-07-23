# Vendored: tebako/tpkg.h

`tpkg.h` is a verbatim copy of the single-header tebako package manifest
mini-lib, vendored — not forked — from its canonical home:

- **Source:** https://raw.githubusercontent.com/tamatebako/libtfs/main/include/tebako/tpkg.h
- **Upstream repo:** https://github.com/tamatebako/libtfs (`include/tebako/tpkg.h`)
- **Vendored at upstream commit:** `9ebad9265ca36cf08b8053514e5d521d6b862609`

## Local delta (pending upstream sync)

This copy currently carries one format addition that has NOT landed in libtfs
yet: `TPKG_FORMAT_RUNTIME` (`format_id` 4 — the fat-package runtime payload
slot) and the matching `tpkg_validate` relaxation
(`format_id <= TPKG_FORMAT_RUNTIME`). The identical change is applied to the
vendored copy in the tebako repo (`include/tebako/tpkg.h`). Once libtfs takes
the format change, re-vendor byte-for-byte per the procedure below and remove
this section.

## Sync procedure

The file is copied byte-for-byte; do not edit it here. To update:

```console
$ curl -fSsLo include/tebako/tpkg.h \
    https://raw.githubusercontent.com/tamatebako/libtfs/main/include/tebako/tpkg.h
```

then update the upstream commit and SHA256 above in the same commit that
refreshes the file. Format changes must land in libtfs first; this repo only
ever tracks them.
