/**
 * @file tpkg-stitch.c
 * @brief tpkg-stitch — append image slots + a tpkg manifest trailer to a base
 *        binary. Build/test helper for three-part tebako packages.
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
 * Usage:
 *   tpkg-stitch --base FILE [--image FILE:MOUNT]... [--runtime-ref REF]
 *               [--launcher-abi N] [--lean] -o OUT
 *
 * Copies --base to OUT, appends each image payload (format_id=AUTO, mount
 * point taken after the LAST ':'), then writes the tpkg trailer. --lean sets
 * TPKG_FLAG_LEAN (default when --runtime-ref is given).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define TPKG_IMPLEMENTATION
#include <tebako/tpkg.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
typedef __int64 off64_t;
typedef int tbs_ssize_t;
#define tbs_lseek _lseeki64
#define tbs_read _read
#define tbs_write _write
#define tbs_close _close
#define tbs_open _open
#define OPEN_BIN O_BINARY
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
typedef off_t off64_t;
typedef ssize_t tbs_ssize_t;
#define tbs_lseek lseek
#define tbs_read read
#define tbs_write write
#define tbs_close close
#define tbs_open open
#define OPEN_BIN 0
#endif

static void usage(FILE* f)
{
  fprintf(f,
          "usage: tpkg-stitch --base FILE [--image FILE:MOUNT]... [--runtime-ref REF]\n"
          "                   [--launcher-abi N] [--lean] [--no-lean] -o OUT\n");
}

static int copy_fd_contents(int in, int out)
{
  char buf[65536];
  for (;;) {
    tbs_ssize_t r = (tbs_ssize_t)tbs_read(in, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (r == 0) {
      return 0;
    }
    {
      char* p = buf;
      tbs_ssize_t left = r;
      while (left > 0) {
        tbs_ssize_t w = (tbs_ssize_t)tbs_write(out, p, (size_t)left);
        if (w <= 0) {
          return -1;
        }
        p += w;
        left -= w;
      }
    }
  }
}

int main(int argc, char** argv)
{
  const char* base = NULL;
  const char* out = NULL;
  const char* runtime_ref = "";
  unsigned long launcher_abi = 1;
  int lean = -1; /* -1 = auto: lean iff runtime_ref given */
  const char* images[TPKG_MAX_SLOTS];
  size_t nimages = 0;
  tpkg_manifest m;
  int fd_out = -1;
  int i;
  size_t k;

  for (i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "--base") == 0 && i + 1 < argc) {
      base = argv[++i];
    } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
      out = argv[++i];
    } else if (strcmp(a, "--runtime-ref") == 0 && i + 1 < argc) {
      runtime_ref = argv[++i];
    } else if (strcmp(a, "--launcher-abi") == 0 && i + 1 < argc) {
      char* end = NULL;
      launcher_abi = strtoul(argv[++i], &end, 10);
      if (!end || *end) {
        fprintf(stderr, "tpkg-stitch: bad --launcher-abi value\n");
        return 2;
      }
    } else if (strcmp(a, "--lean") == 0) {
      lean = 1;
    } else if (strcmp(a, "--no-lean") == 0) {
      lean = 0;
    } else if (strcmp(a, "--image") == 0 && i + 1 < argc) {
      if (nimages >= TPKG_MAX_SLOTS) {
        fprintf(stderr, "tpkg-stitch: too many images (max %u)\n", (unsigned)TPKG_MAX_SLOTS);
        return 2;
      }
      images[nimages++] = argv[++i];
    } else {
      usage(stderr);
      return 2;
    }
  }

  if (!base || !out || nimages == 0) {
    usage(stderr);
    return 2;
  }
  if (lean < 0) {
    lean = runtime_ref[0] != '\0';
  }

  memset(&m, 0, sizeof(m));
  m.version = TPKG_VERSION;
  m.package_flags = lean ? TPKG_FLAG_LEAN : 0;
  m.slot_count = (uint32_t)nimages;
  m.launcher_abi = (uint32_t)launcher_abi;
  if (strlen(runtime_ref) >= TPKG_RUNTIME_REF_LEN) {
    fprintf(stderr, "tpkg-stitch: runtime_ref too long (max %u)\n", (unsigned)TPKG_RUNTIME_REF_LEN - 1);
    return 2;
  }
  strcpy(m.runtime_ref, runtime_ref);

  fd_out = tbs_open(out, O_WRONLY | O_CREAT | O_TRUNC | OPEN_BIN, 0755);
  if (fd_out < 0) {
    fprintf(stderr, "tpkg-stitch: cannot create %s: %s\n", out, strerror(errno));
    return 1;
  }

  /* base payload */
  {
    int fd_in = tbs_open(base, O_RDONLY | OPEN_BIN, 0);
    if (fd_in < 0 || copy_fd_contents(fd_in, fd_out) != 0) {
      fprintf(stderr, "tpkg-stitch: cannot copy base %s: %s\n", base, strerror(errno));
      if (fd_in >= 0) {
        tbs_close(fd_in);
      }
      tbs_close(fd_out);
      return 1;
    }
    tbs_close(fd_in);
  }

  /* image slots */
  for (k = 0; k < nimages; k++) {
    const char* spec = images[k];
    const char* colon = strrchr(spec, ':'); /* last ':' — tolerates C:\ paths */
    char file[4096];
    size_t flen;
    int fd_in;
    off64_t off;
    off64_t size;
    if (!colon || colon == spec || !colon[1]) {
      fprintf(stderr, "tpkg-stitch: bad --image spec \"%s\" (want FILE:MOUNT)\n", spec);
      tbs_close(fd_out);
      return 2;
    }
    flen = (size_t)(colon - spec);
    if (flen >= sizeof(file) || strlen(colon + 1) >= TPKG_MOUNT_POINT_LEN) {
      fprintf(stderr, "tpkg-stitch: --image spec too long: \"%s\"\n", spec);
      tbs_close(fd_out);
      return 2;
    }
    memcpy(file, spec, flen);
    file[flen] = '\0';
    fd_in = tbs_open(file, O_RDONLY | OPEN_BIN, 0);
    if (fd_in < 0) {
      fprintf(stderr, "tpkg-stitch: cannot open image %s: %s\n", file, strerror(errno));
      tbs_close(fd_out);
      return 1;
    }
    size = tbs_lseek(fd_in, 0, SEEK_END);
    off = tbs_lseek(fd_out, 0, SEEK_END);
    if (size < 0 || off < 0 || copy_fd_contents(fd_in, fd_out) != 0) {
      fprintf(stderr, "tpkg-stitch: cannot append image %s: %s\n", file, strerror(errno));
      tbs_close(fd_in);
      tbs_close(fd_out);
      return 1;
    }
    tbs_close(fd_in);
    m.slots[k].offset = (uint64_t)off;
    m.slots[k].size = (uint64_t)size;
    m.slots[k].format_id = TPKG_FORMAT_AUTO;
    m.slots[k].flags = 0;
    strcpy(m.slots[k].mount_point, colon + 1);
  }

  if (tpkg_write_fd(fd_out, &m) != 0) {
    fprintf(stderr, "tpkg-stitch: cannot write manifest: %s\n", tpkg_strerror(tpkg_errno()));
    tbs_close(fd_out);
    return 1;
  }
  if (tbs_close(fd_out) != 0) {
    fprintf(stderr, "tpkg-stitch: cannot finish %s: %s\n", out, strerror(errno));
    return 1;
  }
  return 0;
}
