# Copyright 2026 RDK Management
# SPDX-License-Identifier: Apache-2.0

set(PLUGIN_NAME CryptoTestBackend)
set(MODULE_NAME ${NAMESPACE}${PLUGIN_NAME})
add_library(${MODULE_NAME} SHARED CryptoTestBackend.cpp Module.cpp)
target_include_directories(${MODULE_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${EYIL_CRYPTO_INCLUDE_DIR})
target_link_libraries(${MODULE_NAME} PUBLIC
    ${NAMESPACE}Core::${NAMESPACE}Core ${NAMESPACE}COM::${NAMESPACE}COM
    ${NAMESPACE}Plugins::${NAMESPACE}Plugins)
target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_NAME=Plugin_CryptoTestBackend BUILD_REFERENCE="development-only")
target_compile_options(${MODULE_NAME} PRIVATE -Wall -Wextra)
eyil_enable_coverage(${MODULE_NAME})

string(TOLOWER "${NAMESPACE}" _plugin_directory)
install(TARGETS ${MODULE_NAME} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/${_plugin_directory}/plugins)
write_config(PLUGINS ${PLUGIN_NAME} CLASSNAME CryptoTestBackend COMPONENT cryptotestbackend)
