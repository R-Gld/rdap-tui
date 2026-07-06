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
  autoconf automake libtool m4 \
  ca-certificates

python3 -m pip install --break-system-packages conan==2.26.2

conan profile detect --force

# Conan Center caches these as prebuilt glibc binaries, which cannot run
# under musl. Point Conan at the apk-installed versions instead so it
# never tries to download or execute its own copies. Only works for
# recipes that request a version range (cmake, pkgconf) -- recipes
# pinning an exact tool version (m4, autoconf, automake) never match
# the platform declaration, so those are force-built from source below.
profile="$(conan profile path default)"
printf '\n[platform_tool_requires]\n' >> "$profile"
for tool in cmake pkgconf; do
  version=$("$tool" --version | head -n1 | awk '{print $NF}')
  printf '%s/%s\n' "$tool" "$version" >> "$profile"
done
conan install . --lockfile=conan.lock --lockfile-partial --build=missing \
  --build=m4 --build=autoconf --build=automake \
  -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release -DRDAP_WARNINGS_AS_ERRORS=ON -DRDAP_STATIC_LINK=ON
cmake --build --preset conan-release --config Release --parallel 2
ctest --preset conan-release -C Release --output-on-failure
