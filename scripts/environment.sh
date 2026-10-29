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

# Set paths for the SDK and Python dependencies used by the CI jobs.
eyil_repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
eyil_deps_dir=${EYIL_DEPS_DIR:-"$eyil_repo_dir/_deps"}
eyil_thunder_prefix=${EYIL_THUNDER_PREFIX:-"$eyil_deps_dir/thunder/install/usr"}
export CMAKE_PREFIX_PATH="$eyil_thunder_prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export LD_LIBRARY_PATH="$eyil_thunder_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$eyil_deps_dir/python${PYTHONPATH:+:$PYTHONPATH}"
