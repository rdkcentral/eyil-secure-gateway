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

#include "SharedBuffers.h"
#include <fcntl.h>
#include <sys/stat.h>

namespace Eyil
{
    SharedBuffers::Lease::~Lease()
    {
        if (record)
            manager->Finish(record, uncertain);
    }

    SharedBuffers::Lease::Lease(Lease &&other) noexcept
        : manager(other.manager)
        , record(std::move(other.record))
        , uncertain(other.uncertain)
    {
    }

    SharedBuffers::Lease &SharedBuffers::Lease::operator=(Lease &&other) noexcept
    {
        if (this != &other)
        {
            if (record)
                manager->Finish(record, uncertain);
            manager = other.manager;
            record = std::move(other.record);
            uncertain = other.uncertain;
        }
        return *this;
    }

    const std::string &SharedBuffers::Lease::Path() const { return record->path; }

    uint32_t SharedBuffers::Lease::Capacity() const { return record->size; }

    bool SharedBuffers::Inside(Region r, uint32_t size) { return r.offset <= size && r.length <= size - r.offset; }

    bool SharedBuffers::Overlap(Region a, Region b)
    {
        return a.length && b.length && uint64_t(a.offset) < uint64_t(b.offset) + b.length
            && uint64_t(b.offset) < uint64_t(a.offset) + a.length;
    }

    unsigned SharedBuffers::Charged(uint32_t uid) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = quotas.find(uid);
        return it == quotas.end() ? 0 : it->second.count;
    }

    SharedBuffers::Allocation SharedBuffers::Allocate(const std::shared_ptr<Connection> &owner, uint32_t size)
    {
        try
        {
            return AllocateImpl(owner, size);
        }
        catch (const std::bad_alloc &)
        {
            log.Error(Reason::Allocation, "buffer_allocation_memory_failure uid=%u", owner->peer.uid);
            return {};
        }
    }

    SharedBuffers::Allocation SharedBuffers::AllocateImpl(const std::shared_ptr<Connection> &owner, uint32_t size)
    {
        Allocation result;
        if (!size || size > MaximumSize)
        {
            result.error = size ? CRYPTO_ERROR_OVERSIZED_INPUT_DATA : CRYPTO_ERROR_INVALID_PARAMETERS;
            log.Error(Reason::Buffer, "allocation_size uid=%u size=%u maximum=%u", owner->peer.uid, size, MaximumSize);
            return result;
        }
        auto record = std::make_shared<Record>();
        record->owner = owner->userId;
        record->uid = owner->peer.uid;
        record->size = size;
        record->id = Token();
        const auto nameToken = Token();
        if (record->id.empty() || nameToken.empty())
        {
            log.Error(Reason::Buffer, "allocation_token_failed uid=%u errno=%d", record->uid, errno);
            return result;
        }
        record->name = "eyil." + nameToken;
        const std::string directory = "/proc/" + std::to_string(owner->peer.pid) + "/root/dev/shm";
        record->path = directory + "/" + record->name;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (owner->closing)
                return result;
            auto &quota = quotas[record->uid];
            if (quota.count >= MaximumCount || owner->closing)
            {
                log.Error(Reason::Buffer, "allocation_quota uid=%u count=%u bytes=%llu requested=%u limit=%u",
                    record->uid, quota.count, static_cast<unsigned long long>(quota.bytes), size, MaximumCount);
                return result;
            }
            try
            {
                records.emplace(record->id, record);
            }
            catch (...)
            {
                if (!quota.count)
                    quotas.erase(record->uid);
                log.Error(Reason::Allocation, "buffer registry allocation failed uid=%u", record->uid);
                return result;
            }
            ++quota.count;
            quota.bytes += size;
        }
        const char *stage = "open_directory";
        int failure = 0;
        const auto prepare = [&]() -> bool
        {
            record->directory.Reset(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (record->directory.Get() < 0)
                return false;
            stage = "create";
            record->fd.Reset(openat(record->directory.Get(), record->name.c_str(),
                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
            if (record->fd.Get() < 0)
                return false;
            record->exists = true;
            stage = "ftruncate";
            if (ftruncate(record->fd.Get(), size))
                return false;
            stage = "fallocate";
            while (fallocate(record->fd.Get(), 0, 0, size))
                if (errno != EINTR)
                    return false;
            stage = "ownership";
            if (fchown(record->fd.Get(), owner->peer.uid, owner->peer.gid) || fchmod(record->fd.Get(), 0600))
                return false;
            stage = "stat";
            struct stat status
            {
            };
            if (fstat(record->fd.Get(), &status))
                return false;
            if (status.st_size != size || !S_ISREG(status.st_mode))
            {
                errno = EINVAL;
                return false;
            }
            stage = "publish";
            result.id = record->id;
            result.name = "/" + record->name;
            result.size = size;
            std::lock_guard<std::mutex> lock(mutex);
            if (owner->closing)
            {
                errno = ECANCELED;
                return false;
            }
            record->published = true;
            return true;
        };
        bool prepared = false;
        try
        {
            // Owned result strings can allocate after the object has been created.
            prepared = prepare();
            if (!prepared)
                failure = errno;
        }
        catch (const std::bad_alloc &)
        {
            failure = ENOMEM;
        }
        catch (...)
        {
            failure = EIO;
        }
        if (prepared)
            result.error = CRYPTO_ERROR_NONE;
        else
        {
            log.Error(Reason::Buffer, "allocation_failed uid=%u stage=%s errno=%d", record->uid, stage, failure);
            {
                std::lock_guard<std::mutex> lock(mutex);
                record->pending = true;
            }
            Finalize(record);
            result.id.clear();
            result.name.clear();
            result.size = 0;
        }
        return result;
    }

    CryptoError SharedBuffers::TryUse(const std::shared_ptr<Connection> &owner, const std::string &id,
        const Region *regions, size_t count, Lease &lease)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = records.find(id);
        if (it == records.end() || it->second->owner != owner->userId || !it->second->published || it->second->active
            || it->second->pending)
        {
            log.Error(Reason::Buffer, "buffer_unavailable uid=%u", owner->peer.uid);
            return CRYPTO_ERROR_INVALID_SESSION_STATE;
        }
        auto &record = it->second;
        for (size_t i = 0; i < count; ++i)
        {
            if (!Inside(regions[i], record->size))
            {
                log.Error(Reason::Buffer, "region_out_of_bounds uid=%u", owner->peer.uid);
                return CRYPTO_ERROR_INVALID_PARAMETERS;
            }
            for (size_t j = 0; j < i; ++j)
                if (Overlap(regions[i], regions[j]))
                {
                    log.Error(Reason::Buffer, "region_overlap uid=%u", owner->peer.uid);
                    return CRYPTO_ERROR_INVALID_PARAMETERS;
                }
        }
        lease.manager = this;
        lease.record = record;
        record->active = true;
        return CRYPTO_ERROR_NONE;
    }

    CryptoError SharedBuffers::Release(const std::shared_ptr<Connection> &owner, const std::string &id)
    {
        std::shared_ptr<Record> record;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = records.find(id);
            if (it == records.end() || it->second->owner != owner->userId || !it->second->published
                || it->second->pending)
            {
                log.Error(Reason::Buffer, "release_invalid_owner_or_state uid=%u", owner->peer.uid);
                return CRYPTO_ERROR_INVALID_SESSION_STATE;
            }
            record = it->second;
            record->pending = true;
        }
        Finalize(record);
        return CRYPTO_ERROR_NONE;
    }

    bool SharedBuffers::Finalize(const std::shared_ptr<Record> &record)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!record->pending || record->active || record->gate || record->finalizing)
                return false;
            if (!records.count(record->id))
                return true;
            record->finalizing = true;
        }
        const bool removed
            = !record->exists || unlinkat(record->directory.Get(), record->name.c_str(), 0) == 0 || errno == ENOENT;
        const int error = errno;
        if (removed)
        {
            record->fd.Reset();
            record->directory.Reset();
        }
        std::lock_guard<std::mutex> lock(mutex);
        record->finalizing = false;
        if (!removed)
        {
            log.Error(Reason::Buffer, "buffer_cleanup_failed uid=%u errno=%d", record->uid, error);
            return false;
        }
        auto it = quotas.find(record->uid);
        --it->second.count;
        it->second.bytes -= record->size;
        if (!it->second.count)
            quotas.erase(it);
        records.erase(record->id);
        return true;
    }

    void SharedBuffers::Finish(const std::shared_ptr<Record> &record, bool uncertain) noexcept
    {
        try
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (uncertain)
                {
                    log.Error(Reason::Buffer, "backend_access_uncertain uid=%u cleanup_deferred=1", record->uid);
                    return;
                }
                record->active = false;
            }
            Finalize(record);
        }
        catch (...)
        {
            log.Error(Reason::Buffer, "finish_use_failed");
        }
    }

    void SharedBuffers::Gate(const std::string &owner)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto &entry : records)
            if (entry.second->owner == owner)
                entry.second->gate = true;
    }

    bool SharedBuffers::Idle(const std::string &owner) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto &entry : records)
            if (entry.second->owner == owner
                && (entry.second->active || entry.second->finalizing
                    || (!entry.second->published && !entry.second->pending)))
                return false;
        return true;
    }

    bool SharedBuffers::Cleanup(const std::string &owner)
    {
        for (;;)
        {
            std::shared_ptr<Record> record;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto &entry : records)
                    if (entry.second->owner == owner)
                    {
                        record = entry.second;
                        break;
                    }
                if (!record)
                    return true;
                record->pending = true;
                record->gate = false;
            }
            if (!Finalize(record))
                return false;
        }
    }
}
