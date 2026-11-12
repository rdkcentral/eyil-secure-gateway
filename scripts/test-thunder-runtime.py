#!/usr/bin/env python3
# Copyright 2026 RDK Management
# SPDX-License-Identifier: Apache-2.0
"""L2: activate the real Gateway wrapper/implementation and an actual ICrypto test plugin."""
import argparse
import json
import os
import re
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def run(args, backend):
    with_crypto = backend != "missing"
    suffix = backend
    args.reports.mkdir(parents=True, exist_ok=True)
    log = args.reports / f"thunder-{suffix}.log"
    with tempfile.TemporaryDirectory(prefix="eyil-thunder-") as directory:
        root = Path(directory)
        (root / "lib").mkdir()
        (root / "plugins").mkdir()
        sources = [args.wrapper, args.implementation, args.mock]
        if args.crypto_test_backend:
            sources.append(args.crypto_test_backend)
        for source in sources:
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
        if backend == "crypto-test-backend":
            crypto_test_backend = json.loads(
                args.crypto_test_backend_configuration.read_text()
            )
            assert crypto_test_backend["callsign"] == "org.rdk.CryptoTestBackend"
            assert crypto_test_backend["classname"] == "CryptoTestBackend"
            crypto_test_backend.update(autostart=True, startuporder=1)
            crypto_test_backend["locator"] = args.crypto_test_backend.name
            config["plugins"].append(crypto_test_backend)
            gateway["configuration"]["backends"] = [
                {"service": "crypto", "callsign": crypto_test_backend["callsign"]}
            ]
        elif with_crypto:
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
                for attempt in range(1 if with_crypto else 6):
                    result = subprocess.run(
                        [
                            str(args.client),
                            (
                                "--crypto-test-backend"
                                if backend == "crypto-test-backend"
                                else (
                                    "--exercise"
                                    if with_crypto
                                    else "--expect-no-crypto"
                                )
                            ),
                        ],
                        check=False,
                        text=True,
                        capture_output=True,
                        timeout=50,
                        env=env,
                    )
                    client_log = args.reports / f"client-{suffix}-{attempt + 1}.log"
                    client_log.write_text(result.stdout + result.stderr)
                    if result.returncode or not re.search(
                        r"SUMMARY passed=\d+ failed=0 .*result=PASS", result.stdout
                    ):
                        raise RuntimeError(
                            f"Diagnostic client failed: {result.stdout}{result.stderr}"
                        )
                    required = [
                        "mixed-sync-async-two-connections",
                        "shared-UID-quota-and-reuse",
                        "buffer-ownership/async",
                        "invalid-request/unknown-service/async",
                        "disconnect-cleanup-and-new-context",
                        "connection-cycle/10",
                        "final-disconnect",
                    ]
                    if backend == "crypto-test-backend":
                        required += [
                            "shared-memory-round-trip/async",
                            "output-overflow",
                            "CRYPTO_OPERATION_EXPORT_KEY/async",
                        ]
                    if not with_crypto:
                        required += [
                            "CRYPTO_OPERATION_VERIFY/missing/async",
                            "CRYPTO_OPERATION_LOAD_KEY/async",
                        ]
                    for case in required:
                        if f"PASS case={case} " not in result.stdout:
                            raise RuntimeError(
                                f"CLI did not execute required case {case}: {result.stdout}"
                            )
                    summary = next(
                        line
                        for line in result.stdout.splitlines()
                        if line.startswith("SUMMARY ")
                    )
                    print(f"{summary}; log={client_log}")
                if backend == "mock":
                    # A different deterministic backend must be reported as a test
                    # failure, while independent cases and final cleanup still run.
                    result = subprocess.run(
                        [str(args.client), "--crypto-test-backend"],
                        check=False,
                        text=True,
                        capture_output=True,
                        timeout=50,
                        env=env,
                    )
                    client_log = args.reports / "client-expected-mismatch.log"
                    client_log.write_text(result.stdout + result.stderr)
                    if (
                        result.returncode != 1
                        or "FAIL case=CRYPTO_OPERATION_GENERATE_KEY/sync"
                        not in result.stdout
                        or "PASS case=final-disconnect " not in result.stdout
                        or "FATAL" in result.stdout
                        or "result=FAIL" not in result.stdout
                    ):
                        raise RuntimeError(
                            f"CLI failure reporting/continuation failed: {result.stdout}{result.stderr}"
                        )
                    print(
                        f"PASS: diagnostic mismatch returns 1, continues independent cases and closes; log={client_log}"
                    )
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
                if "EyilGateway ERROR" in line and "ICrypto lookup failed" in line
            ]
            # Inspect BackendLookup (category 3) only: the diagnostic now also
            # deliberately triggers protocol and shared-buffer validation errors.
            # Summary lines aggregate suppressed errors rather than tracing calls.
            summaries = [
                line
                for line in contents.splitlines()
                if "EyilGateway ERROR" in line
                and "category=3 " in line
                and "last_connection=" in line
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
        print(f"PASS: Thunder activation, {backend} ICrypto, shutdown; log={log}")


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
    parser.add_argument("--crypto-test-backend", type=Path)
    parser.add_argument("--crypto-test-backend-configuration", type=Path)
    args = parser.parse_args()
    if bool(args.crypto_test_backend) != bool(args.crypto_test_backend_configuration):
        parser.error(
            "CryptoTestBackend library and configuration must be supplied together"
        )
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
    run(args, "mock")
    run(args, "missing")
    if args.crypto_test_backend:
        run(args, "crypto-test-backend")
    # All owned Thunder processes have stopped. Verify fatal startup handling
    # against the absent endpoint; this is the ordinary client connect deadline.
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = ":".join(
        args.library_dir + [env.get("LD_LIBRARY_PATH", "")]
    )
    result = subprocess.run(
        [str(args.client), "--exercise"],
        check=False,
        text=True,
        capture_output=True,
        timeout=25,
        env=env,
    )
    log = args.reports / "client-expected-unavailable.log"
    log.write_text(result.stdout + result.stderr)
    if (
        result.returncode != 1
        or "FATAL cannot establish connection" not in result.stdout
        or "SUMMARY passed=0 failed=1" not in result.stdout
        or "RUN case=" in result.stdout
    ):
        raise RuntimeError(
            f"CLI did not abort after failed startup: {result.stdout}{result.stderr}"
        )
    print(f"PASS: unavailable Gateway aborts diagnostic with exit 1; log={log}")


if __name__ == "__main__":
    main()
