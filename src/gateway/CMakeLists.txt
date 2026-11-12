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
    PATH_SUFFIXES include/${NAMESPACE}
)

if(NOT EYIL_CONFIGURATION_INCLUDE_DIR)
    message(FATAL_ERROR
        "IConfiguration.h is required. Install matching ThunderInterfaces or entservices-apis "
        "headers into the SDK, or set EYIL_CONFIGURATION_INCLUDE_DIR."
    )
endif()

find_path(EYIL_CRYPTO_INCLUDE_DIR NAMES interfaces/ICrypto.h
    HINTS "$ENV{EYIL_DEPS_DIR}/crypto/include" "${PROJECT_SOURCE_DIR}/_deps/crypto/include"
    PATH_SUFFIXES include/${NAMESPACE})
if(NOT EYIL_CRYPTO_INCLUDE_DIR)
    message(FATAL_ERROR "ICrypto from entservices-apis topic/crypto is required; set EYIL_CRYPTO_INCLUDE_DIR")
endif()
# IConfiguration and ICrypto must use the same entservices-apis Ids.h.
set(EYIL_CONFIGURATION_INCLUDE_DIR "${EYIL_CRYPTO_INCLUDE_DIR}")
if(NOT PLUGIN_EYILGATEWAY_MODE STREQUAL "Off")
    message(FATAL_ERROR "The initial implementation supports only in-process mode Off")
endif()
add_library(EyilGatewayRuntime STATIC
    GatewayRuntime.cpp CryptoServiceAdapter.cpp SharedBuffers.cpp TransportIdentity.cpp)
target_include_directories(EyilGatewayRuntime PUBLIC ${EYIL_CRYPTO_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(EyilGatewayRuntime PUBLIC
    ${NAMESPACE}Core::${NAMESPACE}Core ${NAMESPACE}COM::${NAMESPACE}COM
    ${NAMESPACE}Plugins::${NAMESPACE}Plugins EyilProtocol::EyilProtocol
    ZeroMQ::ZeroMQ Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(EyilGatewayRuntime PRIVATE -Wall -Wextra)
eyil_enable_coverage(EyilGatewayRuntime)

mark_as_advanced(EYIL_CONFIGURATION_INCLUDE_DIR)

add_library(${MODULE_NAME} SHARED
    EyilGateway.cpp
    Module.cpp
)

target_link_libraries(${MODULE_NAME}
    PRIVATE
        ${NAMESPACE}Core::${NAMESPACE}Core
        ${NAMESPACE}COM::${NAMESPACE}COM
        ${NAMESPACE}Plugins::${NAMESPACE}Plugins
)

target_include_directories(${MODULE_NAME} PRIVATE ${EYIL_CONFIGURATION_INCLUDE_DIR})

target_compile_definitions(${MODULE_NAME} PRIVATE BUILD_REFERENCE="${PROJECT_VERSION}")
target_compile_options(${MODULE_NAME} PRIVATE -Wall -Wextra)

eyil_enable_coverage(${MODULE_NAME})

add_library(${PLUGIN_IMPLEMENTATION} SHARED
    EyilGatewayImplementation.cpp
    Module.cpp
)

target_link_libraries(${PLUGIN_IMPLEMENTATION}
    PRIVATE
        EyilGatewayRuntime
        ${NAMESPACE}Core::${NAMESPACE}Core
        ${NAMESPACE}COM::${NAMESPACE}COM
        ${NAMESPACE}Plugins::${NAMESPACE}Plugins
        EyilProtocol::EyilProtocol
        ZeroMQ::ZeroMQ
)

target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ${EYIL_CONFIGURATION_INCLUDE_DIR})

target_compile_definitions(${PLUGIN_IMPLEMENTATION}
    PRIVATE
        BUILD_REFERENCE="${PROJECT_VERSION}"
        MODULE_NAME=Plugin_EyilGatewayImplementation
)

target_compile_options(${PLUGIN_IMPLEMENTATION} PRIVATE -Wall -Wextra)

eyil_enable_coverage(${PLUGIN_IMPLEMENTATION})

string(TOLOWER "${NAMESPACE}" _plugin_directory)

install(TARGETS ${MODULE_NAME} ${PLUGIN_IMPLEMENTATION}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/${_plugin_directory}/plugins
)

# Preserve the product's cache value so disabling development tools restores the
# empty default, and an explicitly selected real backend always takes precedence.
set(EYIL_CONFIG_CRYPTO_CALLSIGN "${PLUGIN_EYILGATEWAY_CRYPTO_CALLSIGN}")
if(BUILD_EYIL_DEV_TOOLS AND EYIL_CONFIG_CRYPTO_CALLSIGN STREQUAL "")
    set(EYIL_CONFIG_CRYPTO_CALLSIGN "org.rdk.CryptoTestBackend")
endif()

write_config(PLUGINS ${PLUGIN_NAME}
    CLASSNAME EyilGateway
    COMPONENT eyilgateway
)
