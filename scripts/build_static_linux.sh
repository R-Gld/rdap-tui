#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds a statically linked Linux binary against musl libc.
# Intended to run inside an Alpine container, e.g.:
#   docker run --rm -v "$PWD:/workspace" -w /workspace alpine:3.24 sh scripts/build_static_linux.sh
set -eu

apk add --no-cache \
  build-base cmake git make \
  python3 py3-pip \
  perl linux-headers pkgconf \
  autoconf automake libtool \
  ca-certificates

python3 -m pip install --break-system-packages conan==2.26.2

conan profile detect --force
cmake_version=$(cmake --version | head -n1 | awk '{print $3}')
printf '\n[platform_tool_requires]\ncmake/%s\n' "$cmake_version" >> "$(conan profile path default)"
conan install . --lockfile=conan.lock --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release -DRDAP_WARNINGS_AS_ERRORS=ON -DRDAP_STATIC_LINK=ON
cmake --build --preset conan-release --config Release --parallel 2
ctest --preset conan-release -C Release --output-on-failure
