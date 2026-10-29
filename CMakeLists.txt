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

project(eyil-secure-gateway VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)
include(services.cmake)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Coverage)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Protobuf 3.15 REQUIRED MODULE)
find_package(ZeroMQ 4.3 REQUIRED)

add_subdirectory(apis/proto)

if(BUILD_EYILCLIENT)
    add_subdirectory(EyilClient)
endif()

if(PLUGIN_EYILGATEWAY)
    find_package(WPEFramework NAMES WPEFramework Thunder REQUIRED)
    find_package(${NAMESPACE}Core REQUIRED)
    find_package(${NAMESPACE}COM REQUIRED)
    find_package(${NAMESPACE}Plugins REQUIRED)

    include(CmakeHelperFunctions)

    add_subdirectory(EyilGateway)
endif()

if(BUILD_TESTING)
    add_subdirectory(Tests)
endif()

install(FILES LICENSE NOTICE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/eyil-secure-gateway
)

install(FILES doc/EyilSecureGateway.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
