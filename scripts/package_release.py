#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import shutil
import stat
import subprocess
from pathlib import Path


def executable(build_dir: Path) -> Path:
    names = {"rdap-tui"}
    candidates = [
        path
        for path in build_dir.rglob("*")
        if path.is_file() and path.name.lower() in names
    ]
    if len(candidates) != 1:
        found = ", ".join(str(path) for path in candidates) or "none"
        raise RuntimeError(f"expected one rdap-tui executable, found: {found}")
    return candidates[0]


def normalized_platform(runner_os: str) -> str:
    values = {"Linux": "linux", "macOS": "macos"}
    if runner_os not in values:
        raise RuntimeError(f"unsupported runner OS: {runner_os}")
    return values[runner_os]


def normalized_architecture(runner_arch: str) -> str:
    values = {"X64": "x86_64", "ARM64": "arm64"}
    if runner_arch not in values:
        raise RuntimeError(f"unsupported runner architecture: {runner_arch}")
    return values[runner_arch]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--runner-os", required=True)
    parser.add_argument("--runner-arch", required=True)
    arguments = parser.parse_args()

    source = executable(arguments.build_dir)
    version_output = subprocess.check_output(
        [str(source.resolve()), "--version"], text=True
    ).strip()
    prefix = "rdap-tui "
    if not version_output.startswith(prefix):
        raise RuntimeError(f"unexpected version output: {version_output}")
    version = version_output.removeprefix(prefix)

    platform = normalized_platform(arguments.runner_os)
    architecture = normalized_architecture(arguments.runner_arch)
    destination_name = f"rdap-tui-v{version}-{platform}-{architecture}"
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    destination = arguments.output_dir / destination_name
    shutil.copy2(source, destination)
    destination.chmod(destination.stat().st_mode | stat.S_IXUSR)
    print(destination)


if __name__ == "__main__":
    main()
