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

set(PLUGIN_NAME EyilGateway)
set(MODULE_NAME ${NAMESPACE}${PLUGIN_NAME})
set(PLUGIN_IMPLEMENTATION ${MODULE_NAME}Implementation)

find_path(EYIL_CONFIGURATION_INCLUDE_DIR
    NAMES interfaces/IConfiguration.h
    PATH_SUFFIXES include/${NAMESPACE})
if(NOT EYIL_CONFIGURATION_INCLUDE_DIR)
    message(FATAL_ERROR
        "IConfiguration.h is required. Install matching ThunderInterfaces or entservices-apis "
        "headers into the SDK, or set EYIL_CONFIGURATION_INCLUDE_DIR.")
endif()
mark_as_advanced(EYIL_CONFIGURATION_INCLUDE_DIR)

add_library(${MODULE_NAME} SHARED EyilGateway.cpp Module.cpp)
target_link_libraries(${MODULE_NAME} PRIVATE
    ${NAMESPACE}Core::${NAMESPACE}Core
    ${NAMESPACE}COM::${NAMESPACE}COM
    ${NAMESPACE}Plugins::${NAMESPACE}Plugins)
target_include_directories(${MODULE_NAME} PRIVATE ${EYIL_CONFIGURATION_INCLUDE_DIR})
target_compile_definitions(${MODULE_NAME} PRIVATE BUILD_REFERENCE="${PROJECT_VERSION}")
target_compile_options(${MODULE_NAME} PRIVATE -Wall -Wextra)
eyil_enable_coverage(${MODULE_NAME})

add_library(${PLUGIN_IMPLEMENTATION} SHARED EyilGatewayImplementation.cpp Module.cpp)
target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE
    ${NAMESPACE}Core::${NAMESPACE}Core
    ${NAMESPACE}COM::${NAMESPACE}COM
    ${NAMESPACE}Plugins::${NAMESPACE}Plugins
    EyilProtocol::EyilProtocol
    ZeroMQ::ZeroMQ)
target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ${EYIL_CONFIGURATION_INCLUDE_DIR})
target_compile_definitions(${PLUGIN_IMPLEMENTATION} PRIVATE
    BUILD_REFERENCE="${PROJECT_VERSION}"
    MODULE_NAME=Plugin_EyilGatewayImplementation)
target_compile_options(${PLUGIN_IMPLEMENTATION} PRIVATE -Wall -Wextra)
eyil_enable_coverage(${PLUGIN_IMPLEMENTATION})

string(TOLOWER "${NAMESPACE}" _plugin_directory)
install(TARGETS ${MODULE_NAME} ${PLUGIN_IMPLEMENTATION}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/${_plugin_directory}/plugins)
write_config(PLUGINS ${PLUGIN_NAME} CLASSNAME EyilGateway
    COMPONENT eyilgateway)
