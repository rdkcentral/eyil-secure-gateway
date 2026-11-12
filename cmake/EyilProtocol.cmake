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

set(EYIL_PROTO_DIR "${PROJECT_SOURCE_DIR}/apis/proto")

set(EYIL_PROTO_FILES
    ${EYIL_PROTO_DIR}/rpc.proto
    ${EYIL_PROTO_DIR}/crypto.proto
)

protobuf_generate_cpp(EYIL_PROTO_SOURCES EYIL_PROTO_HEADERS
    ${EYIL_PROTO_FILES}
)

add_library(eyilprotocol SHARED ${EYIL_PROTO_SOURCES} ${EYIL_PROTO_HEADERS})
add_library(EyilProtocol::EyilProtocol ALIAS eyilprotocol)

set_target_properties(eyilprotocol PROPERTIES
    VERSION 2.0.0
    SOVERSION 2
    EXPORT_NAME EyilProtocol
)

target_include_directories(eyilprotocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<BUILD_INTERFACE:${EYIL_PROTO_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/eyilprotocol>
)

# One shared implementation of the generated C++ bindings per process. Consumers
# link this target instead of compiling another copy of the same .pb.cc files.
target_link_libraries(eyilprotocol PUBLIC protobuf::libprotobuf)

install(TARGETS eyilprotocol
    EXPORT EyilProtocolTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES ${EYIL_PROTO_HEADERS} ${EYIL_PROTO_DIR}/ProtocolVersion.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eyilprotocol
)
install(FILES ${EYIL_PROTO_FILES}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/eyil/proto
)

configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/EyilProtocolConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/EyilProtocolConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EyilProtocol
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/EyilProtocolConfigVersion.cmake
    VERSION 2.0.0
    COMPATIBILITY SameMajorVersion
)
install(EXPORT EyilProtocolTargets
    NAMESPACE EyilProtocol::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EyilProtocol
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/EyilProtocolConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/EyilProtocolConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EyilProtocol
)
