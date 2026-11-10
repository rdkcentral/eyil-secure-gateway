#!/usr/bin/env python3
# Copyright 2026 RDK Management
# SPDX-License-Identifier: Apache-2.0
"""L2: activate the real Gateway wrapper/implementation and an actual ICrypto test plugin."""
import argparse
import json
import os
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def run(args, with_crypto):
    suffix = "mock" if with_crypto else "missing"
    args.reports.mkdir(parents=True, exist_ok=True)
    log = args.reports / f"thunder-{suffix}.log"
    with tempfile.TemporaryDirectory(prefix="eyil-thunder-") as directory:
        root = Path(directory)
        (root / "lib").mkdir()
        (root / "plugins").mkdir()
        for source in (args.wrapper, args.implementation, args.mock):
            (root / "lib" / source.name).symlink_to(source.resolve())
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        gateway = json.loads(args.configuration.read_text())
        gateway.update(autostart=True, startuporder=2)
        gateway["configuration"]["root"]["locator"] = args.implementation.name
        gateway["configuration"]["backends"] = [
            {"service": "crypto", "callsign": "org.rdk.Crypto"}
        ]
        config = {
            "version": "4.4",
            "legacyinitialize": False,
            "port": port,
            "binding": "127.0.0.1",
            "ipv6": False,
            "persistentpath": str(root / "persistent") + "/",
            "volatilepath": str(root / "volatile") + "/",
            "datapath": str(root / "data") + "/",
            "configs": str(root / "plugins") + "/",
            "systempath": str(root / "lib") + "/",
            "communicator": str(root / "rpc"),
            "process": {
                "priority": 0,
                "policy": "OTHER",
                "oomadjust": 0,
                "stacksize": 0,
            },
            "plugins": [
                {"callsign": "Controller", "configuration": {"subsystems": [""]}}
            ],
        }
        if with_crypto:
            config["plugins"].append(
                {
                    "callsign": "org.rdk.Crypto",
                    "classname": "EyilCryptoMock",
                    "locator": args.mock.name,
                    "autostart": True,
                    "startuporder": 1,
                }
            )
        config["plugins"].append(gateway)
        configuration = root / "config.json"
        configuration.write_text(json.dumps(config))
        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = ":".join(
            args.library_dir + [env.get("LD_LIBRARY_PATH", "")]
        )
        with log.open("w") as output:
            process = subprocess.Popen(
                [str(args.thunder), "-c", str(configuration), "-f"],
                stdin=subprocess.PIPE,
                stdout=output,
                stderr=subprocess.STDOUT,
                env=env,
            )
            try:
                deadline = time.monotonic() + 10
                while "EyilGateway ready" not in log.read_text():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(
                            f"Thunder failed to activate Gateway: {log.read_text()}"
                        )
                    time.sleep(0.05)
                for _ in range(1 if with_crypto else 6):
                    result = subprocess.run(
                        [
                            str(args.client),
                            "--exercise" if with_crypto else "--expect-no-crypto",
                        ],
                        check=True,
                        text=True,
                        capture_output=True,
                        timeout=50,
                        env=env,
                    )
                    print(result.stdout, end="")
            finally:
                if process.poll() is None:
                    process.stdin.write(b"Q\n")
                    process.stdin.flush()
                    try:
                        process.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        raise RuntimeError(
                            "Thunder failed to quiesce Gateway during orderly shutdown"
                        )
        contents = log.read_text()
        if process.returncode != 0 or "EyilGateway deinitialized" not in contents:
            raise RuntimeError(f"Gateway lifecycle failure: {contents}")
        if not with_crypto:
            error_lines = [
                line
                for line in contents.splitlines()
                if "EyilGateway ERROR" in line and "last_connection=" not in line
            ]
            # Suppression summaries preserve ERROR severity. They are aggregate
            # counters, not another log for each failed backend invocation.
            summaries = [
                line
                for line in contents.splitlines()
                if "EyilGateway ERROR" in line and "last_connection=" in line
            ]
            if (
                len(error_lines) > 4
                or not summaries
                or not any(
                    "suppressed=" in line and "total=" in line for line in summaries
                )
            ):
                raise RuntimeError(
                    f"Repeated missing-Crypto failures flooded logs: {contents}"
                )
        print(
            f"PASS: Thunder activation, {'mock ICrypto' if with_crypto else 'missing ICrypto'}, shutdown; log={log}"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in (
        "thunder",
        "wrapper",
        "implementation",
        "mock",
        "configuration",
        "client",
        "reports",
    ):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--library-dir", action="append", default=[])
    args = parser.parse_args()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
        probe.settimeout(1)
        try:
            probe.connect("/tmp/app-eyil-gateway.ipc")
        except (FileNotFoundError, ConnectionRefusedError):
            pass
        else:
            raise RuntimeError(
                "An existing Gateway is using the test endpoint; refusing to replace it"
            )
    run(args, True)
    run(args, False)


if __name__ == "__main__":
    main()
