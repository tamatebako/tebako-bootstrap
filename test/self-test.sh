#!/bin/sh
# self-test.sh — end-to-end self-test for tebako-bootstrap.
#
# Builds a fake release mirror (a fake "runtime" that prints its argv plus a
# manifest.json / SHA256SUMS.txt), stitches a lean package out of the real
# launcher + a fake image slot, then verifies:
#   1. download path        — bootstrap resolves, verifies, installs, execs
#   2. cache-hit path       — second run works with the mirror gone, offline
#   3. offline miss         — exit 69, names runtime_ref + knobs
#   4. SHA256 mismatch      — exit 70, cache untouched
#   5. launcher ABI mismatch— exit 66
#   6. no trailer           — exit 65
#   7. corrupt trailer      — exit 65
#
# Usage: self-test.sh <binary-dir>   (dir holding tebako-bootstrap, tpkg-stitch,
# fake-runtime). POSIX sh; runs under Git Bash on Windows.

set -eu

BIN=${1:?usage: self-test.sh <binary-dir>}

fail() {
  echo "self-test FAIL: $*" >&2
  exit 1
}

say() {
  printf '\n== %s ==\n' "$*"
}

# exe suffix + platform string, derived independently of the C code so the
# test cross-checks the launcher's compile-time mapping.
UNAME_S=$(uname -s 2>/dev/null || echo unknown)
UNAME_M=$(uname -m 2>/dev/null || echo unknown)
EXE=
case $UNAME_S in
  MINGW* | MSYS* | CYGWIN* | Windows_NT)
    EXE=.exe
    # All paths handed to native exes below are already Windows-form; stop
    # MSYS2 from rewriting "FILE:MOUNT" specs as PATH lists (: -> ;).
    export MSYS2_ARG_CONV_EXCL='*'
    ;;
esac

case $UNAME_S in
  Linux)
    case $UNAME_M in
      x86_64 | amd64) MACHINE=x86_64 ;;
      aarch64 | arm64) MACHINE=arm64 ;;
      *) fail "unsupported test arch: $UNAME_M" ;;
    esac
    if ldd --version 2>&1 | grep -qi musl; then
      PLAT=linux-musl-$MACHINE
    else
      PLAT=linux-gnu-$MACHINE
    fi
    ;;
  Darwin)
    case $UNAME_M in
      arm64 | aarch64) PLAT=macos-arm64 ;;
      x86_64) PLAT=macos-x86_64 ;;
      *) fail "unsupported test arch: $UNAME_M" ;;
    esac
    ;;
  MINGW* | MSYS* | CYGWIN* | Windows_NT) PLAT=windows-x86_64 ;;
  *) fail "unsupported test OS: $UNAME_S" ;;
esac

BOOTSTRAP=$BIN/tebako-bootstrap$EXE
STITCH=$BIN/tpkg-stitch$EXE
FAKE=$BIN/fake-runtime$EXE
for f in "$BOOTSTRAP" "$STITCH" "$FAKE"; do
  [ -f "$f" ] || fail "missing binary: $f"
done

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  else
    openssl dgst -sha256 -r "$1" | cut -d' ' -f1
  fi
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

TEBAKO_VER=9.9.9
RUBY_VER=3.3.7
REF="ruby@$RUBY_VER;tebako=$TEBAKO_VER"
ASSET="tebako-runtime-$TEBAKO_VER-$RUBY_VER-$PLAT$EXE"
ENTRY="ruby-$RUBY_VER-$TEBAKO_VER-$PLAT"

say "setup: fake release mirror for $ASSET"
MIRRORROOT=$WORK/mirror
MIRROR=$MIRRORROOT/v$TEBAKO_VER
mkdir -p "$MIRROR"
cp "$FAKE" "$MIRROR/$ASSET"
SHA=$(sha256_of "$MIRROR/$ASSET")
printf '%s  %s\n' "$SHA" "$ASSET" >"$MIRROR/SHA256SUMS.txt"
cat >"$MIRROR/manifest.json" <<EOF
[
  {
    "tebako_version": "$TEBAKO_VER",
    "ruby_version": "$RUBY_VER",
    "platform": "$PLAT",
    "filename": "$ASSET",
    "sha256": "$SHA",
    "size_bytes": 12345
  }
]
EOF

say "setup: stitch lean package"
echo "FAKE TFS IMAGE PAYLOAD" >"$WORK/app.tfs"
PKG=$WORK/myapp$EXE
"$STITCH" --base "$BOOTSTRAP" --image "$WORK/app.tfs:/__tebako_memfs__" \
  --runtime-ref "$REF" -o "$PKG"
chmod +x "$PKG"

run_pkg() {
  env TEBAKO_HOME="$WORK/home" TEBAKO_RUNTIME_MIRROR="$MIRRORROOT" "$PKG" hello "arg two"
}

say "1: download path"
OUT=$(run_pkg)
echo "$OUT"
echo "$OUT" | grep -q -e 'FAKE-RUNTIME' || fail "fake runtime did not run"
echo "$OUT" | grep -q -e ':0:/__tebako_memfs__' || fail "missing --tebako-image <self>:0:<mount>"
echo "$OUT" | grep -q -e '--tebako-image' || fail "missing --tebako-image flag"
echo "$OUT" | grep -q -e '--tebako-entry' || fail "missing --tebako-entry"
echo "$OUT" | grep -q -e 'arg two' || fail "argv passthrough broken"
CACHED=$WORK/home/runtimes/$ENTRY/$ASSET
[ -f "$CACHED" ] || fail "runtime not installed into cache: $CACHED"
[ -f "$WORK/home/runtimes/$ENTRY/sha256" ] || fail "sha256 metadata missing"
[ -f "$WORK/home/runtimes/$ENTRY/origin" ] || fail "origin metadata missing"

say "2: cache-hit path (mirror removed, offline)"
mv "$MIRRORROOT" "$WORK/mirror-gone"
OUT=$(TEBAKO_OFFLINE=1 TEBAKO_HOME="$WORK/home" "$PKG" hello "arg two")
echo "$OUT" | grep -q -e 'FAKE-RUNTIME' || fail "cache-hit run failed"

say "3: offline miss"
set +e
OUT=$(TEBAKO_OFFLINE=1 TEBAKO_HOME=$WORK/home2 "$PKG" 2>&1)
RC=$?
set -e
echo "$OUT"
[ "$RC" -eq 69 ] || fail "offline miss: want exit 69, got $RC"
echo "$OUT" | grep -q -e "$REF" || fail "offline error does not name runtime_ref"
echo "$OUT" | grep -q -e 'TEBAKO_RUNTIME_MIRROR' || fail "offline error does not name the mirror knob"

say "4: SHA256 mismatch refused"
BADSHA=0000000000000000000000000000000000000000000000000000000000000000
sed "s/$SHA/$BADSHA/" "$WORK/mirror-gone/v$TEBAKO_VER/manifest.json" >"$WORK/bad-manifest.json"
mkdir -p "$MIRRORROOT/v$TEBAKO_VER"
cp "$WORK/mirror-gone/v$TEBAKO_VER/$ASSET" "$MIRRORROOT/v$TEBAKO_VER/$ASSET"
cp "$WORK/bad-manifest.json" "$MIRRORROOT/v$TEBAKO_VER/manifest.json"
set +e
OUT=$(TEBAKO_HOME=$WORK/home3 TEBAKO_RUNTIME_MIRROR=$MIRRORROOT "$PKG" 2>&1)
RC=$?
set -e
echo "$OUT"
[ "$RC" -eq 70 ] || fail "sha mismatch: want exit 70, got $RC"
echo "$OUT" | grep -q -i -e 'sha256' || fail "sha error does not mention SHA256"
[ ! -e "$WORK/home3/runtimes/$ENTRY" ] || fail "mismatched runtime entered the cache"

say "5: launcher ABI mismatch"
ABIPKG=$WORK/abi99$EXE
"$STITCH" --base "$BOOTSTRAP" --image "$WORK/app.tfs:/__tebako_memfs__" \
  --runtime-ref "$REF" --launcher-abi 99 -o "$ABIPKG"
chmod +x "$ABIPKG"
set +e
OUT=$("$ABIPKG" 2>&1)
RC=$?
set -e
echo "$OUT"
[ "$RC" -eq 66 ] || fail "ABI mismatch: want exit 66, got $RC"
echo "$OUT" | grep -q -e '99' || fail "ABI error does not name required ABI"

say "6: no trailer"
set +e
OUT=$("$BOOTSTRAP" 2>&1)
RC=$?
set -e
echo "$OUT"
[ "$RC" -eq 65 ] || fail "no trailer: want exit 65, got $RC"
echo "$OUT" | grep -q -i -e 'manifest' || fail "no-trailer error does not mention the manifest"

say "7: corrupt trailer"
CORRPKG=$WORK/corrupt$EXE
"$STITCH" --base "$BOOTSTRAP" --image "$WORK/app.tfs:/__tebako_memfs__" \
  --runtime-ref "$REF" -o "$CORRPKG"
chmod +x "$CORRPKG"
# flip one byte inside the trailer header's crc field (last 4 bytes of file)
printf '\377' | dd of="$CORRPKG" bs=1 seek=$(($(stat -c %s "$CORRPKG" 2>/dev/null || stat -f %z "$CORRPKG") - 4)) conv=notrunc 2>/dev/null
set +e
OUT=$("$CORRPKG" 2>&1)
RC=$?
set -e
echo "$OUT"
[ "$RC" -eq 65 ] || fail "corrupt trailer: want exit 65, got $RC"

say "PASS: all self-test scenarios succeeded (platform $PLAT)"
