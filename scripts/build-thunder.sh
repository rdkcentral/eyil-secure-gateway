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
deps_dir=${EYIL_DEPS_DIR:-"$repo_dir/_deps"}
prefix="$deps_dir/thunder/install/usr"
jobs=${BUILD_JOBS:-4}
mkdir -p "$deps_dir/thunder"
export PYTHONPATH="$deps_dir/python${PYTHONPATH:+:$PYTHONPATH}"
if ! python3 -c 'import jsonref' >/dev/null 2>&1; then
    python3 -m pip install --target "$deps_dir/python" jsonref==1.1.0
fi

checkout() {
    local name=$1 revision=$2
    local path="$deps_dir/thunder/$name"
    if [[ ! -d "$path/.git" ]]; then
        git init -q "$path"
        git -C "$path" remote add origin "https://github.com/rdkcentral/$name.git"
        git -C "$path" fetch --depth 1 origin "$revision"
        git -C "$path" checkout --detach -q FETCH_HEAD
    elif [[ "$(git -C "$path" rev-parse HEAD)" != "$revision" ]]; then
        printf 'Unexpected %s revision; choose a fresh EYIL_DEPS_DIR\n' "$name" >&2
        exit 1
    fi
}

# Native Thunder R4.4 SDK.
checkout ThunderTools d5dd83c7c19c49c7f25c558c126500bd2d64f7a4
checkout Thunder 2c0fcc5529e7da734be558ca6efa05d934dcce31
checkout ThunderInterfaces 3e0ec2430e8f25b53eb5bcdfcaf61138b299d966
cmake -S "$deps_dir/thunder/ThunderTools" -B "$deps_dir/thunder/build-tools" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DGENERIC_CMAKE_MODULE_PATH="$prefix/lib/cmake"
cmake --build "$deps_dir/thunder/build-tools" --parallel "$jobs"
cmake --install "$deps_dir/thunder/build-tools"
cmake -S "$deps_dir/thunder/Thunder" -B "$deps_dir/thunder/build" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
    -DCMAKE_MODULE_PATH="$prefix/lib/cmake" -DCMAKE_BUILD_TYPE=Debug \
    -DEXCEPTIONS_ENABLE=ON -DMESSAGING=ON -DHIDE_NON_EXTERNAL_SYMBOLS=OFF \
    -DBINDING=127.0.0.1 -DPORT=9998
cmake --build "$deps_dir/thunder/build" --parallel "$jobs"
cmake --install "$deps_dir/thunder/build"
cmake -S "$deps_dir/thunder/ThunderInterfaces" -B "$deps_dir/thunder/build-interfaces" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
    -DCMAKE_MODULE_PATH="$prefix/lib/cmake" \
    -DGENERATOR_SEARCH_PATH="$prefix/include/WPEFramework" \
    -DINTERFACES_PATTERNS="IConfiguration.h Ids.h" -DJSONRPC_PATTERNS="__none__"
cmake --build "$deps_dir/thunder/build-interfaces" --parallel "$jobs"
cmake --install "$deps_dir/thunder/build-interfaces"
