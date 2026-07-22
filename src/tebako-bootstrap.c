/**
 * @file tebako-bootstrap.c
 * @brief The tebako bootstrap launcher — part A of the three-part tebako
 *        package model.
 *
 * Copyright (c) 2026 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 * This file is a part of the Tebako project (tebako-bootstrap).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * ============================================================================
 * What this is (see README.md for the full contract):
 *
 *   A lean tebako package = [tebako-bootstrap][.tfs image slots][tpkg trailer].
 *   This launcher, when executed as such a package:
 *     1. finds its own executable path;
 *     2. parses the tpkg manifest trailer at EOF (vendored tebako/tpkg.h);
 *     3. checks the trailer's launcher_abi against TEBAKO_BOOTSTRAP_LAUNCHER_ABI;
 *     4. parses runtime_ref "type@version;tebako=<abi>";
 *     5. resolves the language runtime — shared cache hit, else download from
 *        the tebako-runtime-ruby GitHub releases (or $TEBAKO_RUNTIME_MIRROR),
 *        SHA256-verified against the release manifest.json (SHA256SUMS.txt
 *        fallback), atomically installed (tmp + rename) under a per-entry
 *        lock;
 *     6. execs the runtime, launcher ABI v1:
 *          <runtime> --tebako-image <self>:<slot>:<mount> ...
 *                    --tebako-entry <argv0> <user args...>
 *
 *   The bootstrap never mounts images and never links libtfs. It downloads
 *   and executes other binaries, so it is deliberately kept small, single-TU,
 *   C99, and free of dependencies beyond the C runtime library. Downloads
 *   are delegated to the curl CLI (present on modern macOS/Linux/Windows 10+)
 *   with a PowerShell fallback on Windows.
 * ============================================================================
 */

/* Feature macros must precede every system header, and tpkg.h's
 * implementation section must come before system headers too (see its
 * banner comment). */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* flock() et al. are hidden by _POSIX_C_SOURCE otherwise */
#endif
#define TPKG_IMPLEMENTATION
#include <tebako/tpkg.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <wchar.h>
#include <windows.h>
typedef int tbs_ssize_t;
#define tbs_read _read
#define tbs_close _close
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
typedef ssize_t tbs_ssize_t;
#define tbs_read read
#define tbs_close close
#endif

/* ---- constants ------------------------------------------------------------ */

#define TEBAKO_BOOTSTRAP_VERSION "0.1.0"
#define TEBAKO_BOOTSTRAP_LAUNCHER_ABI 1u

/* Exit codes (documented in README.md). */
enum {
  EX_TEBAKO_MANIFEST = 65,   /* manifest missing/corrupt/invalid */
  EX_TEBAKO_ABI = 66,        /* launcher ABI mismatch */
  EX_TEBAKO_RUNTIME_REF = 67,/* runtime_ref missing/unparsable */
  EX_TEBAKO_UNAVAILABLE = 69,/* runtime unavailable (offline/network/lock) */
  EX_TEBAKO_SHA = 70,        /* SHA256 mismatch — download refused */
  EX_TEBAKO_IO = 74          /* local I/O failure */
};

#define DEFAULT_RELEASES_BASE \
  "https://github.com/tamatebako/tebako-runtime-ruby/releases/download"

#define PATH_BUF 4096
#define URL_BUF 8192
#define LOCK_TIMEOUT_MS 120000u /* concurrent-install wait budget */
#define LOCK_POLL_MS 200u

/* ---- diagnostics ------------------------------------------------------------ */

static int fail(int code, const char* fmt, ...)
{
  va_list ap;
  fprintf(stderr, "tebako-bootstrap: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  return code;
}

/* checked snprintf: 0 ok, -1 truncation/error */
static int xsnprintf(char* buf, size_t cap, const char* fmt, ...)
{
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(buf, cap, fmt, ap);
  va_end(ap);
  return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static uint64_t now_ms(void)
{
#if defined(_WIN32)
  return (uint64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
  nanosleep(&ts, NULL);
#endif
}

/* ---- platform identity ------------------------------------------------------ */

/* Runtime-package platform string; must match tebako-runtime-ruby's asset
 * naming. glibc vs musl is a compile-time property (__GLIBC__). */
static const char* platform_string(void)
{
#if defined(_WIN32)
  return "windows-x86_64";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  return "macos-arm64";
#else
  return "macos-x86_64";
#endif
#elif defined(__linux__)
#if defined(__GLIBC__)
#if defined(__x86_64__)
  return "linux-gnu-x86_64";
#elif defined(__aarch64__)
  return "linux-gnu-arm64";
#else
#error "unsupported linux-gnu architecture"
#endif
#else
#if defined(__x86_64__)
  return "linux-musl-x86_64";
#elif defined(__aarch64__)
  return "linux-musl-arm64";
#else
#error "unsupported linux-musl architecture"
#endif
#endif
#else
#error "unsupported platform"
#endif
}

static const char* exe_suffix(void)
{
#if defined(_WIN32)
  return ".exe";
#else
  return "";
#endif
}

/* ---- path / fs helpers ------------------------------------------------------ */

#if defined(_WIN32)
/* UTF-8 first; CRT argv is ANSI, so fall back to the ANSI code page. */
static wchar_t* widen(const char* s)
{
  wchar_t* w;
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
  DWORD flags = MB_ERR_INVALID_CHARS;
  UINT cp = CP_UTF8;
  if (n <= 0) {
    cp = CP_ACP;
    flags = 0;
    n = MultiByteToWideChar(cp, 0, s, -1, NULL, 0);
  }
  if (n <= 0) {
    return NULL;
  }
  w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
  if (!w) {
    return NULL;
  }
  if (MultiByteToWideChar(cp, flags, s, -1, w, n) <= 0) {
    free(w);
    return NULL;
  }
  return w;
}

static char* narrow(const wchar_t* ws)
{
  int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
  char* s;
  if (n <= 0) {
    return NULL;
  }
  s = (char*)malloc((size_t)n);
  if (!s) {
    return NULL;
  }
  if (WideCharToMultiByte(CP_UTF8, 0, ws, -1, s, n, NULL, NULL) <= 0) {
    free(s);
    return NULL;
  }
  return s;
}
#endif /* _WIN32 */

static int self_path(char* buf, size_t cap)
{
#if defined(_WIN32)
  wchar_t wbuf[32768];
  DWORD n = GetModuleFileNameW(NULL, wbuf, (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])));
  char* u8;
  char* p;
  if (n == 0 || n >= sizeof(wbuf) / sizeof(wbuf[0])) {
    return -1;
  }
  u8 = narrow(wbuf);
  if (!u8) {
    return -1;
  }
  if (strlen(u8) >= cap) {
    free(u8);
    return -1;
  }
  strcpy(buf, u8);
  free(u8);
  for (p = buf; *p; p++) {
    if (*p == '\\') {
      *p = '/';
    }
  }
  return 0;
#elif defined(__APPLE__)
  {
    uint32_t sz = (uint32_t)cap;
    char* resolved;
    if (_NSGetExecutablePath(buf, &sz) != 0) {
      return -1; /* buffer too small */
    }
    resolved = realpath(buf, NULL);
    if (!resolved) {
      return -1;
    }
    if (strlen(resolved) >= cap) {
      free(resolved);
      return -1;
    }
    strcpy(buf, resolved);
    free(resolved);
    return 0;
  }
#else
  {
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) {
      return -1;
    }
    buf[n] = '\0';
    return 0;
  }
#endif
}

static int os_mkdir(const char* path)
{
#if defined(_WIN32)
  wchar_t* w = widen(path);
  int rc = w ? _wmkdir(w) : -1;
  int e = errno;
  free(w);
  return (rc == 0 || e == EEXIST) ? 0 : -1;
#else
  return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1;
#endif
}

static int mkdir_p(const char* path)
{
  char tmp[PATH_BUF];
  char* p;
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(tmp)) {
    return -1;
  }
  strcpy(tmp, path);
  p = tmp + 1;
  if (tmp[0] && tmp[1] == ':') {
    p = tmp + 2; /* leave drive prefixes ("C:") alone */
  }
  for (; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (os_mkdir(tmp) != 0) {
        return -1;
      }
      *p = '/';
    }
  }
  return os_mkdir(tmp);
}

static int file_exists(const char* path)
{
#if defined(_WIN32)
  wchar_t* w = widen(path);
  struct _stat st;
  int rc = w ? _wstat(w, &st) : -1;
  free(w);
  return rc == 0;
#else
  struct stat st;
  return stat(path, &st) == 0;
#endif
}

static int os_open_read(const char* path)
{
#if defined(_WIN32)
  wchar_t* w = widen(path);
  int fd = w ? _wopen(w, O_RDONLY | O_BINARY, 0) : -1;
  free(w);
  return fd;
#else
  return open(path, O_RDONLY);
#endif
}

static int os_open_write(const char* path)
{
#if defined(_WIN32)
  wchar_t* w = widen(path);
  int fd = w ? _wopen(w, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, _S_IREAD | _S_IWRITE) : -1;
  free(w);
  return fd;
#else
  return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
#endif
}

static int os_remove(const char* path)
{
#if defined(_WIN32)
  wchar_t* w = widen(path);
  int rc = w ? _wremove(w) : -1;
  free(w);
  return rc;
#else
  return remove(path);
#endif
}

static int os_rmdir(const char* path)
{
#if defined(_WIN32)
  wchar_t* w = widen(path);
  int rc = w ? _wrmdir(w) : -1;
  free(w);
  return rc;
#else
  return rmdir(path);
#endif
}

/* rename; fails if dst exists — exactly the atomic-install semantics wanted */
static int os_rename(const char* src, const char* dst)
{
#if defined(_WIN32)
  wchar_t* ws = widen(src);
  wchar_t* wd = widen(dst);
  BOOL ok = (ws && wd) ? MoveFileExW(ws, wd, 0) : FALSE;
  free(ws);
  free(wd);
  return ok ? 0 : -1;
#else
  return rename(src, dst);
#endif
}

static void make_executable(const char* path)
{
#if !defined(_WIN32)
  chmod(path, 0755);
#else
  (void)path;
#endif
}

static int copy_file(const char* src, const char* dst)
{
  int in = os_open_read(src);
  int out;
  char buf[65536];
  tbs_ssize_t r;
  if (in < 0) {
    return -1;
  }
  out = os_open_write(dst);
  if (out < 0) {
    tbs_close(in);
    return -1;
  }
  while ((r = tbs_read(in, buf, sizeof(buf))) > 0) {
    char* p = buf;
    tbs_ssize_t left = r;
    while (left > 0) {
#if defined(_WIN32)
      tbs_ssize_t w = (tbs_ssize_t)_write(out, p, (unsigned int)left);
#else
      tbs_ssize_t w = (tbs_ssize_t)write(out, p, (size_t)left);
#endif
      if (w <= 0) {
        tbs_close(in);
        tbs_close(out);
        os_remove(dst);
        return -1;
      }
      p += w;
      left -= w;
    }
  }
  tbs_close(in);
  if (tbs_close(out) != 0 || r < 0) {
    os_remove(dst);
    return -1;
  }
  return 0;
}

/* ---- SHA-256 (FIPS 180-4), self-contained ---------------------------------- */

typedef struct {
  uint32_t h[8];
  uint64_t total;
  uint8_t block[64];
  size_t used;
} sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t x, unsigned n)
{
  return (x >> n) | (x << (32u - n));
}

static void sha256_transform(sha256_ctx* c, const uint8_t* p)
{
  uint32_t w[64];
  uint32_t a, b, d, e, f, g, h, t1, t2;
  uint32_t cc;
  int i;
  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) |
           (uint32_t)p[i * 4 + 3];
  }
  for (i = 16; i < 64; i++) {
    uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = c->h[0];
  b = c->h[1];
  cc = c->h[2];
  d = c->h[3];
  e = c->h[4];
  f = c->h[5];
  g = c->h[6];
  h = c->h[7];
  for (i = 0; i < 64; i++) {
    uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    t1 = h + s1 + ch + sha256_k[i] + w[i];
    t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c->h[0] += a;
  c->h[1] += b;
  c->h[2] += cc;
  c->h[3] += d;
  c->h[4] += e;
  c->h[5] += f;
  c->h[6] += g;
  c->h[7] += h;
}

static void sha256_init(sha256_ctx* c)
{
  static const uint32_t h0[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  memcpy(c->h, h0, sizeof(h0));
  c->total = 0;
  c->used = 0;
}

static void sha256_update(sha256_ctx* c, const void* data, size_t n)
{
  const uint8_t* p = (const uint8_t*)data;
  c->total += (uint64_t)n;
  while (n > 0) {
    size_t take = 64 - c->used;
    if (take > n) {
      take = n;
    }
    memcpy(c->block + c->used, p, take);
    c->used += take;
    p += take;
    n -= take;
    if (c->used == 64) {
      sha256_transform(c, c->block);
      c->used = 0;
    }
  }
}

static void sha256_final(sha256_ctx* c, uint8_t out[32])
{
  uint64_t bits = c->total * 8u;
  uint8_t pad = 0x80;
  uint8_t zero = 0;
  uint8_t lenbe[8];
  int i;
  sha256_update(c, &pad, 1);
  while (c->used != 56) {
    sha256_update(c, &zero, 1);
  }
  for (i = 0; i < 8; i++) {
    lenbe[i] = (uint8_t)(bits >> (56 - 8 * i));
  }
  sha256_update(c, lenbe, 8);
  for (i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(c->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(c->h[i]);
  }
}

static int sha256_file_hex(const char* path, char hex[65])
{
  static const char* digits = "0123456789abcdef";
  sha256_ctx c;
  uint8_t digest[32];
  uint8_t buf[65536];
  tbs_ssize_t r;
  int fd = os_open_read(path);
  int i;
  if (fd < 0) {
    return -1;
  }
  sha256_init(&c);
  while ((r = tbs_read(fd, buf, sizeof(buf))) > 0) {
    sha256_update(&c, buf, (size_t)r);
  }
  tbs_close(fd);
  if (r < 0) {
    return -1;
  }
  sha256_final(&c, digest);
  for (i = 0; i < 32; i++) {
    hex[i * 2] = digits[digest[i] >> 4];
    hex[i * 2 + 1] = digits[digest[i] & 15];
  }
  hex[64] = '\0';
  return 0;
}

static int valid_hex_n(const char* s, size_t n)
{
  size_t i;
  for (i = 0; i < n; i++) {
    char ch = s[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
      return 0;
    }
  }
  return 1;
}

static void lower_hex(char* s)
{
  for (; *s; s++) {
    if (*s >= 'A' && *s <= 'F') {
      *s = (char)(*s - 'A' + 'a');
    }
  }
}

/* ---- download helper -------------------------------------------------------- */

/* curl CLI (present on modern macOS/Linux/Windows 10+); PowerShell fallback
 * on Windows. Returns 0 on success, -1 otherwise. */
static int fetch_http(const char* url, const char* out)
{
#if defined(_WIN32)
  {
    wchar_t* wurl = widen(url);
    wchar_t* wout = widen(out);
    const wchar_t* cargv[10];
    intptr_t rc;
    if (!wurl || !wout) {
      free(wurl);
      free(wout);
      return -1;
    }
    cargv[0] = L"curl";
    cargv[1] = L"-fSLsS";
    cargv[2] = L"--retry";
    cargv[3] = L"3";
    cargv[4] = L"--connect-timeout";
    cargv[5] = L"30";
    cargv[6] = L"-o";
    cargv[7] = wout;
    cargv[8] = wurl;
    cargv[9] = NULL;
    rc = _wspawnvp(_P_WAIT, L"curl", cargv);
    if (rc == -1 && (errno == ENOENT || errno == E2BIG)) {
      const wchar_t* pargv[8];
      pargv[0] = L"powershell";
      pargv[1] = L"-NoProfile";
      pargv[2] = L"-NonInteractive";
      pargv[3] = L"-Command";
      pargv[4] = L"Invoke-WebRequest -UseBasicParsing -Uri $args[0] -OutFile $args[1]";
      pargv[5] = wurl;
      pargv[6] = wout;
      pargv[7] = NULL;
      rc = _wspawnvp(_P_WAIT, L"powershell", pargv);
    }
    free(wurl);
    free(wout);
    return rc == 0 ? 0 : -1;
  }
#else
  {
    pid_t pid = fork();
    int status;
    if (pid == 0) {
      execlp("curl", "curl", "-fSLsS", "--retry", "3", "--connect-timeout", "30", "-o", out, url, (char*)NULL);
      _exit(127);
    }
    if (pid < 0) {
      return -1;
    }
    while (waitpid(pid, &status, 0) < 0) {
      if (errno != EINTR) {
        return -1;
      }
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
  }
#endif
}

/* ---- release checksum extraction ------------------------------------------- */

static char* read_text(const char* path)
{
  int fd = os_open_read(path);
  size_t cap = 1u << 20;
  size_t len = 0;
  char* buf;
  if (fd < 0) {
    return NULL;
  }
  buf = (char*)malloc(cap + 1);
  if (!buf) {
    tbs_close(fd);
    return NULL;
  }
  for (;;) {
    tbs_ssize_t r;
    if (len == cap) {
      char* nb;
      if (cap >= (8u << 20)) {
        free(buf);
        tbs_close(fd);
        return NULL; /* implausibly large checksum file */
      }
      cap *= 2;
      nb = (char*)realloc(buf, cap + 1);
      if (!nb) {
        free(buf);
        tbs_close(fd);
        return NULL;
      }
      buf = nb;
    }
    r = tbs_read(fd, buf + len, cap - len);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(buf);
      tbs_close(fd);
      return NULL;
    }
    if (r == 0) {
      break;
    }
    len += (size_t)r;
  }
  tbs_close(fd);
  buf[len] = '\0';
  return buf;
}

/* manifest.json: array of {"filename": ..., "sha256": ...} objects; the asset
 * name is unique in the document, so: locate it, bound the enclosing object,
 * then read that object's "sha256". */
static int sha_from_manifest_json(const char* text, const char* asset, char hex[65])
{
  char needle[512];
  const char* p;
  const char* obj;
  const char* end;
  const char* k;
  const char* q;
  if (xsnprintf(needle, sizeof(needle), "\"%s\"", asset) != 0) {
    return -1;
  }
  p = strstr(text, needle);
  if (!p) {
    return -1;
  }
  obj = p;
  while (obj > text && *obj != '{') {
    obj--;
  }
  if (*obj != '{') {
    return -1;
  }
  end = strchr(p, '}');
  if (!end) {
    return -1;
  }
  k = strstr(obj, "\"sha256\"");
  if (!k || k > end) {
    return -1;
  }
  k += 8;
  while (k < end && (*k == ':' || *k == ' ' || *k == '\t' || *k == '\n' || *k == '\r')) {
    k++;
  }
  if (k >= end || *k != '"') {
    return -1;
  }
  k++;
  q = strchr(k, '"');
  if (!q || q > end || (size_t)(q - k) != 64 || !valid_hex_n(k, 64)) {
    return -1;
  }
  memcpy(hex, k, 64);
  hex[64] = '\0';
  return 0;
}

/* SHA256SUMS.txt fallback: "<64hex><spaces>[*]<filename>" per line */
static int sha_from_sums(const char* text, const char* asset, char hex[65])
{
  size_t alen = strlen(asset);
  const char* line = text;
  while (line && *line) {
    const char* eol = strchr(line, '\n');
    size_t llen = eol ? (size_t)(eol - line) : strlen(line);
    if (llen > 66 && valid_hex_n(line, 64) && (line[64] == ' ' || line[64] == '\t')) {
      const char* name = line + 64;
      size_t nlen;
      while (*name == ' ' || *name == '\t') {
        name++;
      }
      if (*name == '*') {
        name++;
      }
      nlen = llen - (size_t)(name - line);
      while (nlen > 0 && (name[nlen - 1] == '\r' || name[nlen - 1] == ' ' || name[nlen - 1] == '\t')) {
        nlen--;
      }
      if (nlen == alen && memcmp(name, asset, alen) == 0) {
        memcpy(hex, line, 64);
        hex[64] = '\0';
        return 0;
      }
    }
    line = eol ? eol + 1 : NULL;
  }
  return -1;
}

/* ---- per-entry install lock ------------------------------------------------- */

typedef struct {
#if defined(_WIN32)
  HANDLE h;
#else
  int fd;
#endif
} entry_lock;

static int lock_acquire(entry_lock* lk, const char* path)
{
  uint64_t deadline = now_ms() + LOCK_TIMEOUT_MS;
#if defined(_WIN32)
  {
    wchar_t* w = widen(path);
    HANDLE h = w ? CreateFileW(w, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
      return -1;
    }
    for (;;) {
      OVERLAPPED ov;
      memset(&ov, 0, sizeof(ov));
      if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        lk->h = h;
        return 0;
      }
      if (GetLastError() != ERROR_LOCK_VIOLATION && GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(h);
        return -1;
      }
      if (now_ms() >= deadline) {
        CloseHandle(h);
        errno = ETIMEDOUT;
        return -1;
      }
      Sleep(LOCK_POLL_MS);
    }
  }
#else
  {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      return -1;
    }
    for (;;) {
      if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        lk->fd = fd;
        return 0;
      }
      if (errno != EWOULDBLOCK && errno != EINTR) {
        tbs_close(fd);
        return -1;
      }
      if (errno != EINTR && now_ms() >= deadline) {
        tbs_close(fd);
        errno = ETIMEDOUT;
        return -1;
      }
      sleep_ms(LOCK_POLL_MS);
    }
  }
#endif
}

static void lock_release(entry_lock* lk)
{
#if defined(_WIN32)
  OVERLAPPED ov;
  memset(&ov, 0, sizeof(ov));
  UnlockFileEx(lk->h, 0, 1, 0, &ov);
  CloseHandle(lk->h);
#else
  flock(lk->fd, LOCK_UN);
  tbs_close(lk->fd);
#endif
}

/* ---- runtime resolution ------------------------------------------------------ */

static int parse_runtime_ref(const char* ref, char* type, size_t tcap, char* ver, size_t vcap, char* abi,
                             size_t acap)
{
  const char* at = strchr(ref, '@');
  const char* semi;
  const char* abiv;
  size_t tl, vl, al;
  if (!at || at == ref) {
    return -1;
  }
  semi = strstr(at + 1, ";tebako=");
  if (!semi || semi == at + 1) {
    return -1;
  }
  abiv = semi + 8;
  if (!*abiv) {
    return -1;
  }
  tl = (size_t)(at - ref);
  vl = (size_t)(semi - (at + 1));
  al = strcspn(abiv, ";"); /* tolerate trailing ;key=val parameters */
  if (tl == 0 || tl >= tcap || vl == 0 || vl >= vcap || al == 0 || al >= acap) {
    return -1;
  }
  memcpy(type, ref, tl);
  type[tl] = '\0';
  memcpy(ver, at + 1, vl);
  ver[vl] = '\0';
  memcpy(abi, abiv, al);
  abi[al] = '\0';
  /* these become path/URL components — refuse anything that could escape */
  if (strpbrk(type, "/\\ \t\r\n") || strpbrk(ver, "/\\ \t\r\n") || strpbrk(abi, "/\\ \t\r\n")) {
    return -1;
  }
  return 0;
}

static int offline_mode(void)
{
  const char* v = getenv("TEBAKO_OFFLINE");
  return v && *v && strcmp(v, "0") != 0;
}

static int cache_root(char* buf, size_t cap)
{
  const char* home = getenv("TEBAKO_HOME");
  if (home && *home) {
    return xsnprintf(buf, cap, "%s", home);
  }
#if defined(_WIN32)
  home = getenv("LOCALAPPDATA");
  if (home && *home) {
    return xsnprintf(buf, cap, "%s/tebako", home);
  }
  home = getenv("USERPROFILE");
  if (home && *home) {
    return xsnprintf(buf, cap, "%s/.tebako", home);
  }
  return -1;
#else
  home = getenv("HOME");
  if (!home || !*home) {
    return -1;
  }
  return xsnprintf(buf, cap, "%s/.tebako", home);
#endif
}

/* remote base (http/https) or local mirror directory */
static const char* releases_base(void)
{
  const char* m = getenv("TEBAKO_RUNTIME_MIRROR");
  if (m && *m) {
    return m;
  }
  return DEFAULT_RELEASES_BASE;
}

static int base_is_local(const char* base)
{
  return !(strncmp(base, "http://", 7) == 0 || strncmp(base, "https://", 8) == 0);
}

static const char* skip_file_scheme(const char* base)
{
  if (strncmp(base, "file://", 7) == 0) {
    return base + 7;
  }
  return base;
}

static int fetch_url(const char* url, int local, const char* out)
{
  if (local) {
    return copy_file(url, out);
  }
  return fetch_http(url, out);
}

static void cleanup_tmp_entry(const char* dir, const char* asset)
{
  char p[PATH_BUF];
  if (xsnprintf(p, sizeof(p), "%s/%s", dir, asset) == 0) {
    os_remove(p);
  }
  if (xsnprintf(p, sizeof(p), "%s/manifest.json", dir) == 0) {
    os_remove(p);
  }
  if (xsnprintf(p, sizeof(p), "%s/SHA256SUMS.txt", dir) == 0) {
    os_remove(p);
  }
  if (xsnprintf(p, sizeof(p), "%s/sha256", dir) == 0) {
    os_remove(p);
  }
  if (xsnprintf(p, sizeof(p), "%s/origin", dir) == 0) {
    os_remove(p);
  }
  os_rmdir(dir);
}

static int write_small_file(const char* path, const char* content)
{
  int fd = os_open_write(path);
  size_t len = strlen(content);
  size_t off = 0;
  if (fd < 0) {
    return -1;
  }
  while (off < len) {
#if defined(_WIN32)
    tbs_ssize_t w = (tbs_ssize_t)_write(fd, content + off, (unsigned int)(len - off));
#else
    tbs_ssize_t w = (tbs_ssize_t)write(fd, content + off, len - off);
#endif
    if (w <= 0) {
      tbs_close(fd);
      return -1;
    }
    off += (size_t)w;
  }
  return tbs_close(fd);
}

/*
 * Resolve the runtime named by runtime_ref to a local executable path.
 * Returns 0 and fills out_path on success; on failure returns the exit code
 * after printing a spec-conformant error.
 */
static int resolve_runtime(const char* runtime_ref, const char* type, const char* ver, const char* abi,
                           char* out_path, size_t out_cap)
{
  const char* platform = platform_string();
  char entry[512];
  char asset[512];
  char root[PATH_BUF];
  char entry_dir[PATH_BUF];
  char exe_path[PATH_BUF];
  char lock_path[PATH_BUF];
  char tmp_dir[PATH_BUF];
  char tmp_asset[PATH_BUF];
  char tmp_aux[PATH_BUF];
  char base_buf[URL_BUF];
  const char* base_raw = releases_base();
  const char* base;
  int local;
  char asset_url[URL_BUF];
  char manifest_url[URL_BUF];
  char sums_url[URL_BUF];
  entry_lock lk;
  char expected[65];
  char actual[65];
  char* text;
  int have_expected = 0;

  if (xsnprintf(entry, sizeof(entry), "%s-%s-%s-%s", type, ver, abi, platform) != 0 ||
      xsnprintf(asset, sizeof(asset), "tebako-runtime-%s-%s-%s%s", abi, ver, platform, exe_suffix()) != 0) {
    return fail(EX_TEBAKO_IO, "runtime_ref components too long: \"%s\"", runtime_ref);
  }
  if (cache_root(root, sizeof(root)) != 0) {
    return fail(EX_TEBAKO_IO, "cannot determine tebako cache root (set TEBAKO_HOME)");
  }
  if (xsnprintf(entry_dir, sizeof(entry_dir), "%s/runtimes/%s", root, entry) != 0 ||
      xsnprintf(exe_path, sizeof(exe_path), "%s/%s", entry_dir, asset) != 0) {
    return fail(EX_TEBAKO_IO, "cache path too long under %s", root);
  }

  /* cache hit */
  if (file_exists(exe_path)) {
    if (xsnprintf(out_path, out_cap, "%s", exe_path) != 0) {
      return fail(EX_TEBAKO_IO, "cache path too long under %s", root);
    }
    return 0;
  }

  /* URLs (built up front so failures can name what was tried) */
  base = skip_file_scheme(base_raw);
  local = base_is_local(base_raw);
  if (xsnprintf(base_buf, sizeof(base_buf), "%s", base) != 0 ||
      xsnprintf(asset_url, sizeof(asset_url), "%s/v%s/%s", base, abi, asset) != 0 ||
      xsnprintf(manifest_url, sizeof(manifest_url), "%s/v%s/manifest.json", base, abi) != 0 ||
      xsnprintf(sums_url, sizeof(sums_url), "%s/v%s/SHA256SUMS.txt", base, abi) != 0) {
    return fail(EX_TEBAKO_IO, "TEBAKO_RUNTIME_MIRROR value too long");
  }

  if (offline_mode()) {
    return fail(EX_TEBAKO_UNAVAILABLE,
                "cannot resolve runtime \"%s\": not present in the cache and TEBAKO_OFFLINE is set\n"
                "  cache entry: %s\n"
                "  would fetch: %s\n"
                "  unset TEBAKO_OFFLINE, or set TEBAKO_RUNTIME_MIRROR to a reachable mirror",
                runtime_ref, entry_dir, asset_url);
  }

  if (xsnprintf(lock_path, sizeof(lock_path), "%s/locks/%s.lock", root, entry) != 0) {
    return fail(EX_TEBAKO_IO, "cache path too long under %s", root);
  }
  {
    char d[PATH_BUF];
    if (xsnprintf(d, sizeof(d), "%s/locks", root) != 0 || mkdir_p(d) != 0 ||
        xsnprintf(d, sizeof(d), "%s/tmp", root) != 0 || mkdir_p(d) != 0 ||
        xsnprintf(d, sizeof(d), "%s/runtimes", root) != 0 || mkdir_p(d) != 0) {
      return fail(EX_TEBAKO_IO, "cannot create tebako cache directories under %s: %s", root, strerror(errno));
    }
  }

  /* serialize concurrent first runs of any tebako app needing this runtime */
  if (lock_acquire(&lk, lock_path) != 0) {
    if (errno == ETIMEDOUT) {
      return fail(EX_TEBAKO_UNAVAILABLE,
                  "timed out after %us waiting for another tebako bootstrap to finish installing \"%s\"\n"
                  "  lock: %s\n"
                  "  if no other tebako process is running, remove the stale lock file",
                  LOCK_TIMEOUT_MS / 1000u, runtime_ref, lock_path);
    }
    return fail(EX_TEBAKO_IO, "cannot acquire install lock %s: %s", lock_path, strerror(errno));
  }

  /* re-check under the lock: another process may have installed it */
  if (file_exists(exe_path)) {
    lock_release(&lk);
    if (xsnprintf(out_path, out_cap, "%s", exe_path) != 0) {
      return fail(EX_TEBAKO_IO, "cache path too long under %s", root);
    }
    return 0;
  }

  /* build the entry in tmp/ on the same filesystem, then rename atomically */
  if (xsnprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp/%s.%ld", root, entry, (long)
#if defined(_WIN32)
                _getpid()
#else
                getpid()
#endif
                    ) != 0 ||
      xsnprintf(tmp_asset, sizeof(tmp_asset), "%s/%s", tmp_dir, asset) != 0) {
    lock_release(&lk);
    return fail(EX_TEBAKO_IO, "cache path too long under %s", root);
  }
  cleanup_tmp_entry(tmp_dir, asset); /* sweep leftovers from a crashed run */
  if (os_mkdir(tmp_dir) != 0) {
    lock_release(&lk);
    return fail(EX_TEBAKO_IO, "cannot create %s: %s", tmp_dir, strerror(errno));
  }

  if (fetch_url(asset_url, local, tmp_asset) != 0) {
    cleanup_tmp_entry(tmp_dir, asset);
    lock_release(&lk);
    return fail(EX_TEBAKO_UNAVAILABLE,
                "cannot resolve runtime \"%s\": download failed\n"
                "  url: %s\n"
                "  the download helper is curl (PowerShell on Windows) — check the network, or set\n"
                "  TEBAKO_RUNTIME_MIRROR to a reachable mirror, or TEBAKO_OFFLINE=1 for cache-only mode",
                runtime_ref, asset_url);
  }

  /* expected checksum: manifest.json primary, SHA256SUMS.txt fallback */
  if (xsnprintf(tmp_aux, sizeof(tmp_aux), "%s/manifest.json", tmp_dir) == 0 &&
      fetch_url(manifest_url, local, tmp_aux) == 0 && (text = read_text(tmp_aux)) != NULL) {
    have_expected = sha_from_manifest_json(text, asset, expected) == 0;
    free(text);
  }
  if (!have_expected && xsnprintf(tmp_aux, sizeof(tmp_aux), "%s/SHA256SUMS.txt", tmp_dir) == 0 &&
      fetch_url(sums_url, local, tmp_aux) == 0 && (text = read_text(tmp_aux)) != NULL) {
    have_expected = sha_from_sums(text, asset, expected) == 0;
    free(text);
  }
  if (!have_expected) {
    cleanup_tmp_entry(tmp_dir, asset);
    lock_release(&lk);
    return fail(EX_TEBAKO_UNAVAILABLE,
                "cannot resolve runtime \"%s\": no checksum for %s in the release\n"
                "  tried: %s\n"
                "         %s",
                runtime_ref, asset, manifest_url, sums_url);
  }

  if (sha256_file_hex(tmp_asset, actual) != 0) {
    cleanup_tmp_entry(tmp_dir, asset);
    lock_release(&lk);
    return fail(EX_TEBAKO_IO, "cannot hash downloaded file %s: %s", tmp_asset, strerror(errno));
  }
  lower_hex(expected);
  if (strcmp(expected, actual) != 0) {
    cleanup_tmp_entry(tmp_dir, asset);
    lock_release(&lk);
    return fail(EX_TEBAKO_SHA,
                "SHA256 mismatch for downloaded runtime %s — refusing to install or execute\n"
                "  expected: %s (from %s)\n"
                "  actual:   %s\n"
                "  the download was deleted; the cache was not touched",
                asset, expected, manifest_url, actual);
  }

  /* complete the entry, then publish atomically */
  make_executable(tmp_asset);
  if (xsnprintf(tmp_aux, sizeof(tmp_aux), "%s/sha256", tmp_dir) == 0) {
    char content[160];
    xsnprintf(content, sizeof(content), "%s  %s\n", actual, asset);
    write_small_file(tmp_aux, content);
  }
  if (xsnprintf(tmp_aux, sizeof(tmp_aux), "%s/origin", tmp_dir) == 0) {
    char content[URL_BUF + 256];
    xsnprintf(content, sizeof(content), "runtime_ref=%s\nurl=%s\nsha256=%s\n", runtime_ref, asset_url, actual);
    write_small_file(tmp_aux, content);
  }
  if (file_exists(entry_dir)) {
    /* directory exists without the executable — an interrupted manual edit;
     * never delete user state behind its back */
    cleanup_tmp_entry(tmp_dir, asset);
    lock_release(&lk);
    return fail(EX_TEBAKO_IO,
                "cache entry %s exists but is incomplete (missing %s)\n"
                "  remove that directory and run again",
                entry_dir, asset);
  }
  if (os_rename(tmp_dir, entry_dir) != 0) {
    cleanup_tmp_entry(tmp_dir, asset);
    lock_release(&lk);
    return fail(EX_TEBAKO_IO, "cannot install runtime into the cache (%s -> %s): %s", tmp_dir, entry_dir,
                strerror(errno));
  }
  lock_release(&lk);

  if (xsnprintf(out_path, out_cap, "%s", exe_path) != 0) {
    return fail(EX_TEBAKO_IO, "cache path too long under %s", root);
  }
  return 0;
}

/* ---- exec handoff (launcher ABI v1) ------------------------------------------ */

static int exec_runtime(const char* runtime, const char* self, const tpkg_manifest* m, int argc, char** argv)
{
  static char image_arg[TPKG_MAX_SLOTS][PATH_BUF + 320];
  char** nargv;
  size_t n;
  size_t i = 0;
  uint32_t s;
  int a;

  n = 1 + (size_t)m->slot_count * 2 + 2 + (size_t)(argc > 1 ? argc - 1 : 0) + 1;
  nargv = (char**)calloc(n, sizeof(char*));
  if (!nargv) {
    return fail(EX_TEBAKO_IO, "out of memory building runtime argv");
  }
  nargv[i++] = (char*)runtime;
  for (s = 0; s < m->slot_count; s++) {
    if (xsnprintf(image_arg[s], sizeof(image_arg[s]), "%s:%u:%s", self, (unsigned)s,
                  m->slots[s].mount_point) != 0) {
      free(nargv);
      return fail(EX_TEBAKO_IO, "image argument too long for slot %u", (unsigned)s);
    }
    nargv[i++] = (char*)"--tebako-image";
    nargv[i++] = image_arg[s];
  }
  nargv[i++] = (char*)"--tebako-entry";
  nargv[i++] = (argc > 0 && argv[0]) ? argv[0] : (char*)self;
  for (a = 1; a < argc; a++) {
    nargv[i++] = argv[a];
  }
  nargv[i] = NULL;

#if defined(_WIN32)
  {
    wchar_t** wargv = (wchar_t**)calloc(i + 1, sizeof(wchar_t*));
    wchar_t* wruntime = widen(runtime);
    size_t j;
    intptr_t rc;
    if (!wargv || !wruntime) {
      free(wargv);
      free(wruntime);
      free(nargv);
      return fail(EX_TEBAKO_IO, "out of memory building runtime argv");
    }
    for (j = 0; j < i; j++) {
      wargv[j] = widen(nargv[j]);
    }
    rc = _wexecv(wruntime, (const wchar_t* const*)wargv);
    /* only reached on failure */
    free(wruntime);
    free(wargv);
    free(nargv);
    return fail(EX_TEBAKO_IO, "cannot execute runtime %s: %s (rc=%ld)", runtime, strerror(errno), (long)rc);
  }
#else
  execv(runtime, nargv);
  /* only reached on failure */
  return fail(EX_TEBAKO_IO, "cannot execute runtime %s: %s", runtime, strerror(errno));
#endif
}

/* ---- main --------------------------------------------------------------------- */

int main(int argc, char** argv)
{
  char self[PATH_BUF];
  tpkg_manifest m;
  int fd;
  int rc;
  int terr;
  char type[64];
  char ver[64];
  char abi[64];
  char runtime[PATH_BUF];

  if (self_path(self, sizeof(self)) != 0) {
    return fail(EX_TEBAKO_IO, "cannot determine own executable path");
  }

  fd = os_open_read(self);
  if (fd < 0) {
    return fail(EX_TEBAKO_IO, "cannot open own executable %s: %s", self, strerror(errno));
  }
  rc = tpkg_read_fd(fd, &m);
  terr = tpkg_errno();
  tbs_close(fd);

  if (rc != 0) {
    if (terr == TPKG_ERR_NO_TRAILER) {
      return fail(EX_TEBAKO_MANIFEST,
                  "%s carries no tebako manifest trailer —\n"
                  "  a bare tebako-bootstrap only becomes runnable when stitched into a\n"
                  "  three-part package (tebakofs bundle --bootstrap … --image …)",
                  self);
    }
    return fail(EX_TEBAKO_MANIFEST, "corrupt tebako manifest trailer in %s (%s) — re-stitch the package", self,
                tpkg_strerror(terr));
  }

  if (m.launcher_abi > TEBAKO_BOOTSTRAP_LAUNCHER_ABI) {
    return fail(EX_TEBAKO_ABI,
                "package requires launcher ABI %u but this tebako-bootstrap %s supports ABI %u —\n"
                "  refresh the runtime via tebako cache, or re-bundle with a current tebako-bootstrap",
                (unsigned)m.launcher_abi, TEBAKO_BOOTSTRAP_VERSION, TEBAKO_BOOTSTRAP_LAUNCHER_ABI);
  }

  if (m.runtime_ref[0] == '\0') {
    return fail(EX_TEBAKO_RUNTIME_REF,
                "package has no runtime_ref (classic bundle?) — nothing for the bootstrap to resolve");
  }
  if (parse_runtime_ref(m.runtime_ref, type, sizeof(type), ver, sizeof(ver), abi, sizeof(abi)) != 0) {
    return fail(EX_TEBAKO_RUNTIME_REF,
                "cannot parse runtime_ref \"%s\" — expected \"<type>@<version>;tebako=<abi>\"", m.runtime_ref);
  }

  rc = resolve_runtime(m.runtime_ref, type, ver, abi, runtime, sizeof(runtime));
  if (rc != 0) {
    return rc;
  }
  return exec_runtime(runtime, self, &m, argc, argv);
}
