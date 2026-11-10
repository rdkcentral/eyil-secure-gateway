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

#include "../common/RuntimeSupport.h"
#include <atomic>
#include <cstdint>
#include <unordered_set>
#include <rpc.pb.h>

namespace Eyil
{
    struct Peer
    {
        uint32_t uid = 0;
        uint32_t gid = 0;
        int32_t pid = 0;
        uint64_t lifetime = 0;
        int fd = -1;
    };

    struct Connection
    {
        Peer peer;
        std::string userId;
        std::string appId;
        std::string sessionId;
        std::atomic<bool> closing { false };

        // Remaining fields are serialized by the Gateway registry mutex.
        bool authenticated = false;
        bool initialized = false;
        bool authFinished = false;
        bool cleanupScheduled = false;
        bool cleanupFinished = false;
        unsigned active = 0;
        std::unordered_set<uint64_t> requests;
    };

    struct ServiceResult
    {
        RpcStatus status = RPC_STATUS_INTERNAL;
        std::string payload;
    };

    class ServiceAdapter
    {
    public:
        virtual ~ServiceAdapter() = default;
        virtual bool Notify(const Connection &, bool connected) = 0;
        virtual ServiceResult Execute(const std::shared_ptr<Connection> &, const std::string &) = 0;
    };
}
