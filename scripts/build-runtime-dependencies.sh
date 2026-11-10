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
revision=6926f6a72fbfff18b691d09b3b7376fbd541b9d3
mkdir -p "$deps_dir/crypto/include/interfaces" "$deps_dir/zmq"
api_base="https://raw.githubusercontent.com/rdkcentral/entservices-apis/$revision/apis"
for item in Crypto/ICrypto.h DisplayInfo/IConfiguration.h Module.h Ids.h entservices_errorcodes.h; do
    destination="$deps_dir/crypto/include/interfaces/${item##*/}"
    curl --fail --location --retry 3 --proto '=https' "$api_base/$item" -o "$destination.tmp"
    if cmp -s "$destination.tmp" "$destination"; then
        rm "$destination.tmp"
    else
        mv "$destination.tmp" "$destination"
    fi
done
archive="$deps_dir/zmq/libzmq-4.3.4.tar.gz"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 --proto '=https' \
        https://codeload.github.com/zeromq/libzmq/tar.gz/refs/tags/v4.3.4 -o "$archive"
fi
printf '%s  %s\n' 0ff5a531c9ffaf0dfdc7dc78d13d1383088f454896d252934c429b2554d10559 "$archive" | sha256sum -c -
source_dir="$deps_dir/zmq/libzmq-4.3.4"
if [[ ! -d "$source_dir" ]]; then
    tar -xzf "$archive" -C "$deps_dir/zmq"
fi
patch_file="$repo_dir/doc/zmq_connection_lifetime.patch"
if patch --batch --forward --dry-run -p1 -d "$source_dir" < "$patch_file" >/dev/null; then
    patch --batch --forward -p1 -d "$source_dir" < "$patch_file"
elif ! patch --batch --forward --reverse --dry-run -p1 -d "$source_dir" < "$patch_file" >/dev/null; then
    printf 'libzmq source does not match the required patch; use a fresh EYIL_DEPS_DIR\n' >&2
    exit 1
fi
cmake -S "$source_dir" -B "$deps_dir/zmq/build" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$deps_dir/zmq/install" -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_DRAFTS=ON -DENABLE_CURVE=OFF -DWITH_LIBSODIUM=OFF \
    -DWITH_PERF_TOOL=OFF -DBUILD_TESTS=ON
cmake --build "$deps_dir/zmq/build" --parallel "${BUILD_JOBS:-4}"
ctest --test-dir "$deps_dir/zmq/build" -R '^test_connection_lifetime$' --output-on-failure --no-tests=error
cmake --install "$deps_dir/zmq/build"
