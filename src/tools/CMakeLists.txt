# Copyright 2026 RDK Management
# SPDX-License-Identifier: Apache-2.0
add_executable(eyil-client EyilClientExample.cpp)
target_link_libraries(eyil-client PRIVATE EyilClient::EyilClient EyilProtocol::EyilProtocol Threads::Threads)
install(TARGETS eyil-client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(EYIL_DEV_SYSROOT "${CMAKE_SYSROOT}" CACHE PATH "Target runtime sysroot for the development client bundle")
if(NOT EYIL_DEV_SYSROOT)
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Cross development bundle requires EYIL_DEV_SYSROOT or CMAKE_SYSROOT")
    endif()
    set(EYIL_DEV_SYSROOT "/")
endif()
if(NOT CMAKE_READELF)
    find_program(CMAKE_READELF readelf REQUIRED)
endif()
install(CODE "
    execute_process(COMMAND \"${Python3_EXECUTABLE}\" \"${PROJECT_SOURCE_DIR}/scripts/stage-dev-client.py\"
        --executable \"$<TARGET_FILE:eyil-client>\"
        --destination \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/eyil\"
        --sysroot \"${EYIL_DEV_SYSROOT}\" --readelf \"${CMAKE_READELF}\"
        --library \"$<TARGET_FILE:eyilclient>\" --library \"$<TARGET_FILE:eyilprotocol>\"
        --library \"$<TARGET_FILE:protobuf::libprotobuf>\" --library \"$<TARGET_FILE:ZeroMQ::ZeroMQ>\"
        --search \"$<TARGET_FILE_DIR:eyilclient>\" --search \"$<TARGET_FILE_DIR:eyilprotocol>\"
        --search \"$<TARGET_FILE_DIR:protobuf::libprotobuf>\" --search \"$<TARGET_FILE_DIR:ZeroMQ::ZeroMQ>\"
        COMMAND_ERROR_IS_FATAL ANY)
")
