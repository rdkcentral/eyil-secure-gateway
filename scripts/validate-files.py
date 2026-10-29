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

"""Validate JSON, text formatting and licensing headers in repository files."""

import json
import subprocess
import sys
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    listing = subprocess.run(
        ["git", "ls-files", "-c", "-o", "--exclude-standard", "-z"],
        cwd=root,
        check=True,
        capture_output=True,
    )
    errors = []
    for raw_name in sorted(set(listing.stdout.split(b"\0")) - {b""}):
        name = raw_name.decode("utf-8")
        path = root / name
        if not path.is_file() or path.is_symlink():
            continue
        data = path.read_bytes()
        text = data.decode("utf-8")
        if name == "README.md":
            if data:
                errors.append("README.md must remain empty")
            continue
        if "\r" in text or not text.endswith("\n"):
            errors.append(f"{name}: use LF with a final newline")
        for index, line in enumerate(text.splitlines(), 1):
            if line.rstrip() != line:
                errors.append(f"{name}:{index}: trailing whitespace")
        if path.suffix == ".json":
            try:
                json.loads(text)
            except ValueError as error:
                errors.append(f"{name}: {error}")
        elif name not in {"LICENSE", "NOTICE"}:
            if "Copyright 2026 RDK Management" not in text[:1800]:
                errors.append(f"{name}: missing licensing header")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("JSON, text formatting and licensing headers are valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
