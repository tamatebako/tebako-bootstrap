# Copyright (c) 2026 [Ribose Inc](https://www.ribose.com).
# All rights reserved.
# This file is a part of the Tebako project (tebako-bootstrap).
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
# TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
# BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

cmake_minimum_required(VERSION 3.16)
project(tebako-bootstrap VERSION 0.2.0 LANGUAGES C)

# The launcher downloads and execs other binaries: keep it small, auditable,
# and free of dependencies beyond the C runtime library (C99).

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(TEBAKO_BOOTSTRAP_STATIC "Link a fully static binary where supported" OFF)

if(MSVC)
  add_compile_options(/W4 /wd4996)
  # Static CRT so the launcher runs on machines without the VC++ redist
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(TEBAKO_BOOTSTRAP_STATIC AND (CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    # Sensible on musl; glibc static linking is possible but discouraged —
    # the option is honored as requested either way.
    add_link_options(-static)
  endif()
endif()

add_executable(tebako-bootstrap src/tebako-bootstrap.c)
target_include_directories(tebako-bootstrap PRIVATE include)

# Test/build helper: appends image slots + tpkg manifest to a base binary.
# Also useful standalone for hand-rolled packages.
add_executable(tpkg-stitch tools/tpkg-stitch.c)
target_include_directories(tpkg-stitch PRIVATE include)

install(TARGETS tebako-bootstrap RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  add_executable(fake-runtime test/fake-runtime.c)
  add_test(
    NAME self-test
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/self-test.sh $<TARGET_FILE_DIR:tebako-bootstrap>
  )
endif()
