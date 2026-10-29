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

"""Run a labeled suite, explicitly recording an empty suite without dummy tests."""

import argparse
import json
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def discover(build_dir, label):
    result = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1", "-L", label],
        check=True,
        capture_output=True,
        text=True,
    )
    return json.loads(result.stdout)["tests"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--label", choices=["L1", "L2", "L1|L2"], default="L1|L2")
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    if not (build_dir / "CTestTestfile.cmake").is_file():
        parser.error("Configure the build with BUILD_TESTING=ON before running tests")

    tests = discover(build_dir, args.label)
    suite = args.label.replace("|", "-")
    report_dir = build_dir / "reports" / suite
    report_dir.mkdir(parents=True, exist_ok=True)
    junit = report_dir / "junit.xml"
    result = subprocess.run(
        [
            "ctest",
            "--test-dir",
            str(build_dir),
            "-L",
            args.label,
            "--output-on-failure",
            "--no-tests=ignore",
            "--output-junit",
            str(junit),
        ],
        check=False,
    )
    if not tests:
        root = ET.Element("testsuites", tests="0", failures="0", errors="0")
        ET.SubElement(
            root, "testsuite", name=suite, tests="0", failures="0", errors="0"
        )
        ET.ElementTree(root).write(junit, encoding="utf-8", xml_declaration=True)
        summary = f"{args.label}: no tests registered; 0 tests executed.\n"
    else:
        summary = f"{args.label}: {len(tests)} tests registered; CTest exit code {result.returncode}.\n"
    (report_dir / "summary.md").write_text(summary, encoding="utf-8")
    print(summary, end="", flush=True)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
