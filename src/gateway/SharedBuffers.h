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
#include <crypto.pb.h>
#include <unordered_map>

namespace Eyil
{
    class SharedBuffers
    {
        struct Record;

    public:
        static constexpr uint32_t MaximumSize = 1572864;
        static constexpr unsigned MaximumCount = 4;

        struct Region
        {
            uint32_t offset;
            uint32_t length;
        };

        struct Allocation
        {
            CryptoError error = CRYPTO_ERROR_OPERATION_FAILED;
            std::string id;
            std::string name;
            uint32_t size = 0;
        };

        class Lease
        {
        public:
            Lease() = default;
            ~Lease();
            Lease(const Lease &) = delete;
            Lease &operator=(const Lease &) = delete;
            Lease(Lease &&other) noexcept;
            Lease &operator=(Lease &&other) noexcept;

            const std::string &Path() const;
            uint32_t Capacity() const;

            void Uncertain() noexcept { uncertain = true; }

            explicit operator bool() const noexcept { return static_cast<bool>(record); }

        private:
            friend class SharedBuffers;

            SharedBuffers *manager = nullptr;
            std::shared_ptr<Record> record;
            bool uncertain = false;
        };

        explicit SharedBuffers(Diagnostics &diagnostics)
            : log(diagnostics)
        {
        }

        Allocation Allocate(const std::shared_ptr<Connection> &owner, uint32_t size);
        CryptoError Release(const std::shared_ptr<Connection> &owner, const std::string &id);
        CryptoError TryUse(const std::shared_ptr<Connection> &owner, const std::string &id, const Region *regions,
            size_t count, Lease &lease);

        void Gate(const std::string &owner);
        bool Idle(const std::string &owner) const;
        bool Cleanup(const std::string &owner);
        unsigned Charged(uint32_t uid) const;

        static bool Inside(Region region, uint32_t size);
        static bool Overlap(Region a, Region b);

    private:
        struct Record
        {
            std::string owner;
            std::string id;
            std::string name;
            std::string path;
            uint32_t uid = 0;
            uint32_t size = 0;
            Fd directory;
            Fd fd;

            bool exists = false;
            bool published = false;
            bool active = false;
            bool pending = false;
            bool gate = false;
            bool finalizing = false;
        };

        struct Quota
        {
            unsigned count = 0;
            uint64_t bytes = 0;
        };

        Allocation AllocateImpl(const std::shared_ptr<Connection> &owner, uint32_t size);
        void Finish(const std::shared_ptr<Record> &record, bool uncertain) noexcept;
        bool Finalize(const std::shared_ptr<Record> &record);

        Diagnostics &log;
        mutable std::mutex mutex;
        std::unordered_map<std::string, std::shared_ptr<Record>> records;
        std::unordered_map<uint32_t, Quota> quotas;
    };
}
