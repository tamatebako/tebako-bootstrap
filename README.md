# tebako-bootstrap

The **tebako bootstrap launcher** — part A of the three-part tebako package
model. A small, language-agnostic, dependency-free C99 executable that forms
the executable portion of *lean* tebako packages. At run time it reads the
package manifest trailer from its own binary, resolves the required language
runtime (cache → download → SHA256-verify → install), and `exec`s it, handing
over the embedded `.tfs` image slots to mount.

This binary downloads and executes other binaries; it is deliberately kept
small and auditable. It links nothing beyond the C runtime library.

## The three-part package model

Every tebako product is a composition of three independently distributable
parts:

| Part | What it is | Distributed as |
|---|---|---|
| **A. tebako bootstrap** (this repo) | Launcher: manifest parser (`tebako/tpkg.h` only, **no libtfs linked**), runtime resolver, exec handoff | `tebako-bootstrap-<tebako-ver>-<platform>` |
| **B. language runtime** | Patched interpreter + tebako-main + libtfs; mounts the images and runs the app | `tebako-runtime-<tebako>-<lang-ver>-<platform>` from [tebako-runtime-ruby](https://github.com/tamatebako/tebako-runtime-ruby) releases |
| **C. data image(s)** | `.tfs` filesystem images (app code, gems, data) | embedded in the package as slots |

### Execution models

- **Classic** (fat, in-process): runtime and image linked into one binary; no
  bootstrap involved, no trailer needed. Unchanged, fully supported.
- **Lean** (three-part): bootstrap + images + manifest trailer
  (`TPKG_FLAG_LEAN` set, `runtime_ref` filled). First run resolves the runtime
  into the machine-wide shared cache; every run ends in `execve(runtime, …)`.
- **Self-installing fat** (three-part): a lean package that also carries the
  runtime package itself as a payload slot (`format_id=TPKG_FORMAT_RUNTIME`,
  never mounted). First run installs the payload into the shared cache —
  SHA256-verified against the `;sha256=` parameter of `runtime_ref` — instead
  of downloading it, so fat packages run with no network access at all.

## What the launcher does

1. Locates its own executable (`/proc/self/exe`, `_NSGetExecutablePath`,
   `GetModuleFileNameW`).
2. Parses the tpkg manifest trailer at EOF
   ([`include/tebako/tpkg.h`](include/tebako/tpkg.h), vendored byte-for-byte
   from [libtfs](https://github.com/tamatebako/libtfs) — see
   [`include/tebako/VENDORED.md`](include/tebako/VENDORED.md)). Corrupt magic
   or CRC → startup error naming the binary; no partial behavior.
3. Checks `launcher_abi` in the trailer against the ABI this build supports
   (currently **1**); a newer package → clear error naming both ABIs.
4. Parses `runtime_ref` — `"<type>@<version>;tebako=<abi>"`, e.g.
   `ruby@3.3.7;tebako=0.15.0`. A fat package's `runtime_ref` additionally
   carries `;sha256=<64 lowercase hex>` — the checksum of the embedded
   runtime payload.
5. Resolves the runtime, in order:
   - **Cache hit** (`$TEBAKO_HOME/runtimes/<type>-<version>-<tebakoabi>-<platform>/`,
     default `$TEBAKO_HOME` = `~/.tebako`, Windows `%LOCALAPPDATA%\tebako`) → exec.
   - **Payload present** (fat package) → per-entry lock, re-check, extract the
     payload slot from the own executable, verify it against the `;sha256=`
     parameter of `runtime_ref`, and atomically install it into the cache —
     no network access needed (honors `TEBAKO_OFFLINE=1`). A payload checksum
     mismatch refuses execution; a populated cache is never re-verified.
   - **Miss** → per-entry lock (flock / `LockFileEx`, with timeout), re-check,
     then download from
     `https://github.com/tamatebako/tebako-runtime-ruby/releases/download/v<tebako>/tebako-runtime-<tebako>-<version>-<platform>[.exe]`,
     fetch `manifest.json` (fallback `SHA256SUMS.txt`) from the same release,
     verify SHA256, and atomically install (build in `tmp/`, `rename`).
     Concurrent first runs from many tebako apps are safe; partial installs
     are impossible.
6. Execs the runtime (launcher ABI v1):

   ```
   <runtime> --tebako-image <self-path>:<slot-index>:<mount-point> … \
             --tebako-entry <original-argv0> <user args…>
   ```

   One `--tebako-image` per image slot (runtime payload slots are never
   handed over). The runtime mounts the images directly out of the
   bootstrap's file — the bootstrap never mounts anything. Parsers
   split the value at the **last two** colons (mount point, then slot index),
   so Windows drive letters in the path are unambiguous.

### Platform strings

| Build platform | `<platform>` |
|---|---|
| Linux glibc x86_64 / arm64 | `linux-gnu-x86_64` / `linux-gnu-arm64` |
| Linux musl x86_64 / arm64 | `linux-musl-x86_64` / `linux-musl-arm64` |
| macOS arm64 / x86_64 | `macos-arm64` / `macos-x86_64` |
| Windows x86_64 | `windows-x86_64` (runtime asset gets `.exe`) |

glibc vs musl is decided at compile time (`__GLIBC__`).

### Download helper

v1 shells out to the **`curl` command-line tool** — present on modern macOS,
Linux, and Windows 10+ (`C:\Windows\System32\curl.exe`) — with a **PowerShell
`Invoke-WebRequest` fallback on Windows**. This matches the approved design
(tebako thrift-free master plan §4.4) and keeps the binary free of library
dependencies: libcurl was considered and rejected for v1 because Windows has
no OS package manager for it without vcpkg. A mirror whose value is not an
`http(s)://` URL is treated as a **local filesystem directory** with the same
`v<tebako>/…` layout — useful for offline mirrors and for the test suite.
Native TLS fetch is on the roadmap.

### Environment

| Variable | Effect |
|---|---|
| `TEBAKO_HOME` | Cache root (default `~/.tebako`, `%LOCALAPPDATA%\tebako` on Windows) |
| `TEBAKO_RUNTIME_MIRROR` | Base URL (or local directory) replacing the GitHub releases download base |
| `TEBAKO_OFFLINE` | When set (any non-empty value except `0`): cache-only mode, never downloads |

### Exit codes

| Code | Meaning |
|---|---|
| `0` | (never returns — `exec`) |
| `65` | manifest missing, corrupt, or structurally invalid |
| `66` | launcher ABI mismatch (package requires newer bootstrap) |
| `67` | `runtime_ref` missing or unparsable (fat: payload checksum parameter missing) |
| `69` | runtime unavailable: not cached and unreachable / offline / lock timeout |
| `70` | SHA256 mismatch — downloaded or embedded payload deleted, cache untouched |
| `74` | local I/O failure (cache not writable, exec failed, …) |

## Build

Requires only a C99 compiler and CMake ≥ 3.16. No other dependencies.

```console
$ cmake -B build -DCMAKE_BUILD_TYPE=Release
$ cmake --build build
$ ./build/tebako-bootstrap        # bare launcher: prints usage-style error, exit 65
```

`TEBAKO_BOOTSTRAP_STATIC=ON` links a fully static binary where the toolchain
supports it (musl, mingw); on glibc/macOS it is ignored. MSVC builds use the
static CRT (`/MT`).

## Self-test

```console
$ cmake -B build -DBUILD_TESTING=ON
$ cmake --build build
$ ctest --test-dir build --output-on-failure
# or directly:
$ ./test/self-test.sh ./build
```

The self-test stitches a lean package out of the freshly built launcher, a
fake runtime (a tiny program that prints its arguments) served from a local
mirror directory, and a fake image slot — then verifies the download path, the
cache-hit path, offline failure, SHA256-mismatch refusal, ABI mismatch, and
the no-trailer error; plus, for fat packages (runtime payload slot): offline
payload install, payload SHA256-mismatch refusal, and the populated-cache-wins
order. See [`test/self-test.sh`](test/self-test.sh).

## Repo layout

```
src/tebako-bootstrap.c   the launcher (single TU, C99)
tools/tpkg-stitch.c      test/build helper: append images + manifest to a base binary
test/fake-runtime.c      fake language runtime for the self-test (prints argv)
test/self-test.sh        end-to-end self-test (POSIX sh; Git Bash on Windows)
include/tebako/tpkg.h    vendored manifest mini-lib (see VENDORED.md)
```

## License

BSD 2-Clause — © 2026 Ribose Inc. See [LICENSE.md](LICENSE.md).
