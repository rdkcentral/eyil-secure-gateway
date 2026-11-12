#!/usr/bin/env bash
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

set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$repo_dir/scripts/environment.sh"
build_dir=$(realpath "${1:-$repo_dir/build}")
stage_dir=$(realpath -m "${2:-$repo_dir/rootfs/all}")
DESTDIR="$stage_dir" cmake --install "$build_dir"
export LD_LIBRARY_PATH="$stage_dir/usr/lib:$LD_LIBRARY_PATH"
cmake -S "$repo_dir/scripts/consumer" -B "$build_dir/consumer" -G Ninja \
    -UEyilClient_DIR -UEYIL_PROTOCOL_LIBRARY -DCMAKE_PREFIX_PATH="$stage_dir/usr" \
    -DEYIL_INSTALL_ROOT="$stage_dir" -DEYIL_BUILD_DIR="$build_dir"
cmake --build "$build_dir/consumer" --parallel "${BUILD_JOBS:-4}"
"$build_dir/consumer/eyil-c-consumer"
"$build_dir/consumer/eyil-protocol-consumer"
export PKG_CONFIG_LIBDIR="$stage_dir/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$stage_dir"
client_flag_string=$(pkg-config --cflags --libs eyilclient)
read -r -a client_flags <<< "$client_flag_string"
cc "$repo_dir/scripts/consumer/main.c" "${client_flags[@]}" -o "$build_dir/consumer/eyil-pkgconfig-consumer"
"$build_dir/consumer/eyil-pkgconfig-consumer"

python3 - "$build_dir" "$stage_dir" <<'PY'
import ctypes
import json
import os
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1])
stage = Path(sys.argv[2])
for entry in (build / "install_manifest.txt").read_text().splitlines():
    path = Path(entry)
    allowed = (
        ".so" in path.name
        or str(path).endswith("/include/eyilclient/EyilClient.h")
        or str(path).endswith("/bin/eyil-client")
        or str(path).endswith("/WPEFramework/plugins/EyilGateway.json")
        or str(path).endswith("/WPEFramework/plugins/CryptoTestBackend.json")
        or (
            path.parent.name == "EyilClient"
            and path.parent.parent.name == "cmake"
            and path.suffix == ".cmake"
        )
        or str(path).endswith("/pkgconfig/eyilclient.pc")
    )
    assert allowed, f"Unexpected install artifact: {entry}"
print("Install manifest contains runtime artifacts and the client SDK, without documentation.")
if "BUILD_EYIL_DEV_TOOLS:BOOL=ON" in (build / "CMakeCache.txt").read_text():
    bundle = stage / "usr/lib/eyil"
    assert (stage / "usr/bin/eyil-client").is_file()
    assert (bundle / "eyil-client").is_file()
    assert (bundle / "run").is_file()
    assert all(path.name in {"eyil-client", "run"} or ".so" in path.name for path in bundle.iterdir())
    subprocess.run([str(bundle / "run"), "--help"], check=True)
    print("Installed container client starts from /usr/lib/eyil with its private libraries.")
if "PLUGIN_EYILGATEWAY:BOOL=ON" in (build / "CMakeCache.txt").read_text():
    config_path = stage / "etc/WPEFramework/plugins/EyilGateway.json"
    config = json.loads(config_path.read_text())
    assert config["callsign"] == "org.rdk.EyilGateway"
    assert config["classname"] == "EyilGateway"
    selected_callsign = next(
        line.split("=", 1)[1]
        for line in (build / "CMakeCache.txt").read_text().splitlines()
        if line.startswith("PLUGIN_EYILGATEWAY_CRYPTO_CALLSIGN:STRING=")
    )
    development = "BUILD_EYIL_DEV_TOOLS:BOOL=ON" in (build / "CMakeCache.txt").read_text()
    if development and not selected_callsign:
        selected_callsign = "org.rdk.CryptoTestBackend"
    assert config["configuration"]["backends"] == [
        {"service": "crypto", "callsign": selected_callsign}
    ]
    assert "cryptocallsign" not in config["configuration"]
    library = stage / "usr/lib/wpeframework/plugins" / config["locator"]
    ctypes.CDLL(str(library), mode=os.RTLD_NOW)
    root = config["configuration"]["root"]
    assert root["mode"] in {"Off", "Local"}
    implementation = library.parent / root["locator"]
    ctypes.CDLL(str(implementation), mode=os.RTLD_NOW)
    print("Installed EyilGateway wrapper and implementation libraries loaded successfully.")
    test_config_path = stage / "etc/WPEFramework/plugins/CryptoTestBackend.json"
    if development:
        test_config = json.loads(test_config_path.read_text())
        assert test_config["callsign"] == "org.rdk.CryptoTestBackend"
        assert test_config["classname"] == "CryptoTestBackend"
        assert test_config["autostart"] is False
        ctypes.CDLL(str(library.parent / test_config["locator"]), mode=os.RTLD_NOW)
        print("Installed development-only CryptoTestBackend plugin loaded successfully.")
    else:
        assert not test_config_path.exists(), "CryptoTestBackend must not be installed without development tools"
PY
