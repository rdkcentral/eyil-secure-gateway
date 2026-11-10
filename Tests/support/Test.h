/**
 * Copyright 2026 RDK Management
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdexcept>
#include <iostream>
#include <thread>
#include <functional>
#include <chrono>
#define CHECK(value)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(value))                                                                                                  \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #value);            \
    } while (false)

inline void Eventually(const std::function<bool()> &predicate, int milliseconds = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!predicate())
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

inline void RequireUnusedEndpoint()
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    CHECK(fd >= 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, "/tmp/app-eyil-gateway.ipc");
    const int result = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    const int error = errno;
    close(fd);
    CHECK(result < 0 && (error == ECONNREFUSED || error == ENOENT));
}
