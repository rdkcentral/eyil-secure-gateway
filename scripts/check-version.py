#!/usr/bin/env python3
###
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2026 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
###

"""Validate the release version and, optionally, a PR changelog change."""

import argparse
import re
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version")
    parser.add_argument("--base")
    parser.add_argument("--head")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    match = re.search(
        r"project\(eyil-secure-gateway VERSION (\d+\.\d+\.\d+)",
        (root / "CMakeLists.txt").read_text(encoding="utf-8"),
    )
    if not match:
        parser.error("Cannot find the CMake project version")
    version = match.group(1)
    if args.version is not None and args.version != version:
        parser.error("Requested release does not match the CMake version")
    if f"#### [{version}]" not in (root / "CHANGELOG.md").read_text(encoding="utf-8"):
        parser.error("Missing changelog entry for this version")
    version_header = (root / "EyilGateway/Version.h").read_text(encoding="utf-8")
    parts = []
    for component in ("MAJOR", "MINOR", "PATCH"):
        number = re.search(
            rf"^#define API_VERSION_NUMBER_{component}\s+(\d+)\s*$",
            version_header,
            re.MULTILINE,
        )
        if not number:
            parser.error(f"Cannot find plugin {component.lower()} version")
        parts.append(number.group(1))
    if ".".join(parts) != version:
        parser.error("Plugin service registration version does not match CMake")
    if bool(args.base) != bool(args.head):
        parser.error("Provide both --base and --head")
    if args.base:
        changes = subprocess.run(
            ["git", "diff", "--name-only", args.base, args.head, "--"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
        if "CHANGELOG.md" not in changes.stdout.splitlines():
            parser.error("Update CHANGELOG.md for release changes")
    print(f"Version {version} is consistent.")


if __name__ == "__main__":
    main()
