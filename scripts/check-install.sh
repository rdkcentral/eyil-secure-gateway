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
mapfile -t install_paths < <(python3 - "$build_dir/CMakeCache.txt" "$stage_dir" <<'PATHS'
from pathlib import Path
import sys
cache = {}
for line in Path(sys.argv[1]).read_text().splitlines():
    if "=" in line and not line.startswith(("//", "#")):
        key, value = line.split("=", 1)
        cache[key.split(":", 1)[0]] = value
prefix = Path(cache["CMAKE_INSTALL_PREFIX"])
stage = Path(sys.argv[2])
for path in (prefix, prefix / cache["CMAKE_INSTALL_LIBDIR"]):
    print(stage / path.relative_to("/"))
PATHS
)
install_prefix=${install_paths[0]}
install_libdir=${install_paths[1]}
export LD_LIBRARY_PATH="$install_libdir:$LD_LIBRARY_PATH"
test -f "$install_libdir/cmake/EyilClient/EyilClientConfig.cmake"
test -f "$install_libdir/cmake/EyilProtocol/EyilProtocolConfig.cmake"
cmake -S "$repo_dir/scripts/consumer" -B "$build_dir/consumer" -G Ninja \
    -DCMAKE_PREFIX_PATH="$install_prefix" \
    -DEyilClient_DIR="$install_libdir/cmake/EyilClient" \
    -DEyilProtocol_DIR="$install_libdir/cmake/EyilProtocol" \
    -UEYIL_PROTOCOL_LIBRARY -UEYIL_BUILD_DIR -UEYIL_INSTALL_ROOT
cmake --build "$build_dir/consumer" --parallel "${BUILD_JOBS:-4}"
"$build_dir/consumer/eyil-c-consumer"
"$build_dir/consumer/eyil-protocol-consumer"
export PKG_CONFIG_LIBDIR="$install_libdir/pkgconfig"
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
cache = {}
for line in (build / "CMakeCache.txt").read_text().splitlines():
    if "=" in line and not line.startswith(("//", "#")):
        key, value = line.split("=", 1)
        cache[key.split(":", 1)[0]] = value
prefix = Path(cache["CMAKE_INSTALL_PREFIX"])
datadir = cache["CMAKE_INSTALL_DATADIR"] or cache["CMAKE_INSTALL_DATAROOTDIR"]
def installed(path):
    return stage / (prefix / path).relative_to("/")

for entry in (build / "install_manifest.txt").read_text().splitlines():
    path = Path(entry)
    allowed = (
        ".so" in path.name
        or str(path).endswith("/include/eyilclient/EyilClient.h")
        or (path.parent.name == "eyilprotocol" and path.name in {"rpc.pb.h", "crypto.pb.h", "ProtocolVersion.h"})
        or (path.parent == prefix / datadir / "eyil/proto" and path.name in {"rpc.proto", "crypto.proto"})
        or str(path).endswith("/bin/eyil-client")
        or str(path).endswith("/WPEFramework/plugins/EyilGateway.json")
        or str(path).endswith("/WPEFramework/plugins/CryptoTestBackend.json")
        or (
            path.parent.name in {"EyilClient", "EyilProtocol"}
            and path.parent.parent.name == "cmake"
            and path.suffix == ".cmake"
        )
        or str(path).endswith("/pkgconfig/eyilclient.pc")
    )
    assert allowed, f"Unexpected install artifact: {entry}"
for name in ("rpc.pb.h", "crypto.pb.h", "ProtocolVersion.h"):
    assert installed(Path(cache["CMAKE_INSTALL_INCLUDEDIR"]) / "eyilprotocol" / name).is_file()
for name in ("rpc.proto", "crypto.proto"):
    assert installed(Path(datadir) / "eyil/proto" / name).is_file()
print("Install manifest contains runtime artifacts and the client/protocol SDKs, without documentation.")
if "BUILD_EYIL_DEV_TOOLS:BOOL=ON" in (build / "CMakeCache.txt").read_text():
    bundle = installed(Path(cache["CMAKE_INSTALL_LIBDIR"]) / "eyil")
    assert installed(Path(cache["CMAKE_INSTALL_BINDIR"]) / "eyil-client").is_file()
    assert (bundle / "eyil-client").is_file()
    assert (bundle / "run").is_file()
    assert all(path.name in {"eyil-client", "run"} or ".so" in path.name for path in bundle.iterdir())
    subprocess.run([str(bundle / "run"), "--help"], check=True)
    print(f"Installed container client starts from {bundle} with its private libraries.")
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
    library = installed("lib/wpeframework/plugins") / config["locator"]
    ctypes.CDLL(str(library), mode=os.RTLD_NOW)
    root = config["configuration"]["root"]
    assert root["mode"] == "Off"
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
