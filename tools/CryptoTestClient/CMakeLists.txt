# Copyright 2026 RDK Management
# SPDX-License-Identifier: Apache-2.0

add_executable(CryptoTestClient CryptoTestClient.cpp
    TestClient.cpp CryptoCases.cpp BufferCases.cpp ClientCases.cpp)
target_compile_options(CryptoTestClient PRIVATE -Wall -Wextra)
target_link_libraries(CryptoTestClient PRIVATE
    EyilClient::EyilClient EyilProtocol::EyilProtocol Threads::Threads rt)
install(TARGETS CryptoTestClient RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
