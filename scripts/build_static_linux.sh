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
# never tries to download or execute its own copies.
#
# conan.lock already pins m4/autoconf/automake to exact versions, and
# lockfile resolution takes priority over --build policy even with
# --lockfile-partial, so forcing a source rebuild of those references
# has no effect. platform_tool_requires works instead because it
# substitutes the requirement itself rather than its build policy, but
# the substitution only matches a *version range* request (cmake,
# pkgconf) -- an exact-pinned request only matches a platform
# declaration for that same exact version, regardless of what apk
# actually installed, so mirror conan.lock's pinned versions below.
profile="$(conan profile path default)"
{
  printf '\n[platform_tool_requires]\n'
  printf 'cmake/%s\n' "$(cmake --version | head -n1 | awk '{print $NF}')"
  printf 'pkgconf/%s\n' "$(pkgconf --version | head -n1 | awk '{print $NF}')"
  printf 'm4/1.4.19\n'
  printf 'autoconf/2.71\n'
  printf 'automake/1.16.5\n'
} >> "$profile"
conan install . --lockfile=conan.lock --lockfile-partial --build=missing \
  -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release -DRDAP_WARNINGS_AS_ERRORS=ON -DRDAP_STATIC_LINK=ON
cmake --build --preset conan-release --config Release --parallel 2
ctest --preset conan-release -C Release --output-on-failure
