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

add_library(eyilclient SHARED src/EyilClient.cpp)
add_library(EyilClient::EyilClient ALIAS eyilclient)

set_target_properties(eyilclient PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    EXPORT_NAME EyilClient
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

target_include_directories(eyilclient
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(eyilclient
    PRIVATE
        EyilProtocol::EyilProtocol
        ZeroMQ::ZeroMQ
)

target_compile_options(eyilclient PRIVATE -Wall -Wextra -Wpedantic)

eyil_enable_coverage(eyilclient)

install(TARGETS eyilclient
    EXPORT EyilClientTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/eyilclient DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_file(eyilclient.pc.in eyilclient.pc @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/eyilclient.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

configure_package_config_file(EyilClientConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/EyilClientConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EyilClient
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/EyilClientConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT EyilClientTargets
    NAMESPACE EyilClient::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EyilClient
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/EyilClientConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/EyilClientConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EyilClient
)
