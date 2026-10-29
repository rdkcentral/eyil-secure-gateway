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
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [[ -n "${EYIL_THUNDER_PREFIX:-}" ]]; then
    if [[ ! -f "$EYIL_THUNDER_PREFIX/lib/cmake/WPEFramework/WPEFrameworkConfig.cmake" ]]; then
        printf 'EYIL_THUNDER_PREFIX does not contain a usable Thunder SDK\n' >&2
        exit 1
    fi
    if [[ ! -f "$EYIL_THUNDER_PREFIX/include/WPEFramework/interfaces/IConfiguration.h" ]]; then
        printf 'The supplied SDK needs matching ThunderInterfaces or entservices-apis IConfiguration headers\n' >&2
        exit 1
    fi
    printf 'Using the supplied Thunder SDK: %s\n' "$EYIL_THUNDER_PREFIX"
    exit 0
fi
exec bash "$repo_dir/scripts/build-thunder.sh"
