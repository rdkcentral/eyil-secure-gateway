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
cd "$repo_dir"
source scripts/environment.sh
export PATH="$eyil_deps_dir/python/bin:$PATH"
python3 scripts/validate-files.py
python3 -m black --check scripts
python3 -m yamllint .github .clang-format .yamllint.yml
mapfile -d '' sources < <(git ls-files -c -o --exclude-standard -z -- '*.h' '*.cpp' '*.c' '*.proto')
if (( ${#sources[@]} )); then
    clang-format --dry-run --Werror "${sources[@]}"
fi
mapfile -d '' scripts < <(git ls-files -c -o --exclude-standard -z -- '*.sh')
for script in "${scripts[@]}"; do
    bash -n "$script"
done
