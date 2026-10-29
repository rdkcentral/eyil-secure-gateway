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

find_path(ZeroMQ_INCLUDE_DIR NAMES zmq.h)
find_library(ZeroMQ_LIBRARY NAMES zmq)

if(ZeroMQ_INCLUDE_DIR)
    file(STRINGS "${ZeroMQ_INCLUDE_DIR}/zmq.h" _zmq_version
        REGEX "^#define ZMQ_VERSION_(MAJOR|MINOR|PATCH) ")
    foreach(part MAJOR MINOR PATCH)
        string(REGEX MATCH "ZMQ_VERSION_${part} +([0-9]+)" _match "${_zmq_version}")
        set(_zmq_${part} "${CMAKE_MATCH_1}")
    endforeach()
    set(ZeroMQ_VERSION "${_zmq_MAJOR}.${_zmq_MINOR}.${_zmq_PATCH}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ZeroMQ
    REQUIRED_VARS ZeroMQ_INCLUDE_DIR ZeroMQ_LIBRARY
    VERSION_VAR ZeroMQ_VERSION)

if(ZeroMQ_FOUND AND NOT TARGET ZeroMQ::ZeroMQ)
    add_library(ZeroMQ::ZeroMQ UNKNOWN IMPORTED)
    set_target_properties(ZeroMQ::ZeroMQ PROPERTIES
        IMPORTED_LOCATION "${ZeroMQ_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZeroMQ_INCLUDE_DIR}")
endif()
mark_as_advanced(ZeroMQ_INCLUDE_DIR ZeroMQ_LIBRARY)
