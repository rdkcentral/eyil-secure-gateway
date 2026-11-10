#!/usr/bin/env python3
# Copyright 2026 RDK Management
# SPDX-License-Identifier: Apache-2.0
"""Stage a development-only, container-mountable client and its ELF dependencies.

Uses readelf, never executes target binaries; supports native and cross installs.
The component's ordinary shared libraries remain in their normal install paths.
"""

import argparse
import re
import shutil
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--sysroot", type=Path, default=Path("/"))
    parser.add_argument("--search", type=Path, action="append", default=[])
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--library", type=Path, action="append", default=[])
    args = parser.parse_args()

    def inspect(path):
        result = subprocess.run(
            [args.readelf, "-h", "-l", "-d", str(path)],
            check=True,
            text=True,
            capture_output=True,
        ).stdout
        machine = re.search(r"Machine:\s*(.+)", result).group(1).strip()
        needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", result)
        interpreter = re.search(r"Requesting program interpreter:\s*(.*?)\]", result)
        soname = re.search(r"\(SONAME\).*?\[(.*?)\]", result)
        return (
            machine,
            needed,
            interpreter.group(1) if interpreter else None,
            soname.group(1) if soname else None,
        )

    search = list(args.search)
    for relative in ("lib", "lib64", "usr/lib", "usr/lib64"):
        directory = args.sysroot / relative
        search.append(directory)
        if directory.is_dir():
            search.extend(path for path in directory.iterdir() if path.is_dir())
    machine, _, interpreter, _ = inspect(args.executable)
    preferred = {}
    for library in args.library:
        architecture, _, _, soname = inspect(library)
        if soname:
            if architecture != machine:
                raise RuntimeError(
                    f"Wrong explicitly linked library architecture: {library}"
                )
            preferred[soname] = library
    if not interpreter:
        raise RuntimeError("Expected an ELF executable with a dynamic interpreter")
    loader = args.sysroot / interpreter.lstrip("/")
    if not loader.is_file():
        raise RuntimeError(f"ELF interpreter missing from sysroot: {loader}")
    if inspect(loader)[0] != machine:
        raise RuntimeError("Loader architecture does not match the client")
    destination = args.destination
    (destination / "lib").mkdir(parents=True, exist_ok=True)
    pending = [
        (args.executable, destination / "eyil-client"),
        (loader, destination / "lib" / loader.name),
    ]
    copied = set()
    while pending:
        source, target = pending.pop()
        if target.name in copied:
            continue
        architecture, needed, _, _ = inspect(source)
        if architecture != machine:
            raise RuntimeError(f"Wrong dependency architecture: {source}")
        shutil.copy2(source.resolve(), target)
        copied.add(target.name)
        for name in needed:
            found = preferred.get(name)
            if found is None:
                for directory in search:
                    candidate = directory / name
                    if candidate.is_file() and inspect(candidate)[0] == machine:
                        found = candidate
                        break
            if found is None:
                raise RuntimeError(
                    f"Cannot resolve {name} required by {source}; supply target --sysroot/--search"
                )
            pending.append((found, destination / "lib" / name))
    script = destination / "run"
    script.write_text(
        "#!/bin/sh\n# Copyright 2026 RDK Management\n# SPDX-License-Identifier: Apache-2.0\n"
        'bundle_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
        f'exec "$bundle_dir/lib/{loader.name}" --library-path "$bundle_dir/lib" "$bundle_dir/eyil-client" "$@"\n'
    )
    script.chmod(0o755)
    print(
        f"Staged development client and {len(copied) - 1} runtime objects: {destination}"
    )


if __name__ == "__main__":
    main()
