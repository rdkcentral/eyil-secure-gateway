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

#include "GatewayTypes.h"
#include <vector>

namespace Eyil
{
    constexpr int ExpectedConnection = 10001;
    using MonitorFunction = int (*)(void *, const char *, uint64_t, int, int);
    MonitorFunction RequireTransportPatch(void *router);

    struct TransportResult
    {
        int error = 0;
        const char *message = nullptr;

        explicit operator bool() const noexcept { return error == 0; }
    };

    TransportResult VerifyTransportPatch(const std::string &temporaryDirectory = {});
    bool Decimal(const char *text, uint64_t &value) noexcept;
    bool Credentials(Message &message, const std::string &address, Peer &peer);

    enum class MonitorRead
    {
        Empty,
        Event,
        Error
    };

    struct MonitorEvent
    {
        uint64_t event = 0;
        std::vector<uint64_t> values;
    };

    MonitorRead ReadMonitor(void *socket, MonitorEvent &event);
}
