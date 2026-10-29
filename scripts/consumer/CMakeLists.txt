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

cmake_minimum_required(VERSION 3.21)
project(EyilInstalledConsumer LANGUAGES C CXX)

find_package(EyilClient CONFIG REQUIRED)
add_executable(eyil-c-consumer main.c)
target_link_libraries(eyil-c-consumer PRIVATE EyilClient::EyilClient)

find_package(EyilProtocol CONFIG REQUIRED)
add_executable(eyil-protocol-consumer protocol.cpp)
target_compile_features(eyil-protocol-consumer PRIVATE cxx_std_17)
target_link_libraries(eyil-protocol-consumer PRIVATE
    EyilProtocol::EyilProtocol
    EyilClient::EyilClient)
