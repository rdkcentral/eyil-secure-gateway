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

"""Run the suites and generate coverage only when real tests are registered."""

import argparse
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--label", choices=["L1", "L2", "L1|L2"], default="L1|L2")
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    root = Path(__file__).resolve().parents[1]
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file() or "ENABLE_COVERAGE:BOOL=ON" not in cache.read_text(
        encoding="utf-8"
    ):
        parser.error("Configure and build with ENABLE_COVERAGE=ON first")
    if not (build_dir / "CTestTestfile.cmake").is_file():
        parser.error("Configure with BUILD_TESTING=ON first")

    report_dir = build_dir / "coverage"
    report_dir.mkdir(parents=True, exist_ok=True)
    for name in ["coverage.xml", "index.html", "coverage.txt"]:
        (report_dir / name).unlink(missing_ok=True)
    discovery = subprocess.run(
        [
            "ctest",
            "--test-dir",
            str(build_dir),
            "--show-only=json-v1",
            "-L",
            args.label,
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    tests = json.loads(discovery.stdout)["tests"]
    # Count only execution in this coverage run.
    for counter in build_dir.rglob("*.gcda"):
        counter.unlink()
    result = subprocess.run(
        [
            sys.executable,
            str(root / "scripts/run-tests.py"),
            "--build-dir",
            str(build_dir),
            "--label",
            args.label,
        ],
        check=False,
    )
    if result.returncode:
        return result.returncode
    if not tests:
        summary = f"Coverage unavailable for {args.label}: 0 tests executed.\n"
        (report_dir / "summary.md").write_text(summary, encoding="utf-8")
        print(summary, end="")
        return 0
    if importlib.util.find_spec("gcovr") is None:
        parser.error("Install gcovr to generate coverage for registered tests")
    subprocess.run(
        [
            sys.executable,
            "-m",
            "gcovr",
            "--root",
            str(root),
            "--filter",
            re.escape(str(root)) + r"/(EyilGateway|EyilClient)/",
            "--xml-pretty",
            "--xml",
            str(report_dir / "coverage.xml"),
            "--html-details",
            str(report_dir / "index.html"),
            "--txt",
            str(report_dir / "coverage.txt"),
            str(build_dir),
        ],
        check=True,
    )
    (report_dir / "summary.md").write_text(
        f"Coverage generated after {len(tests)} registered {args.label} tests.\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
