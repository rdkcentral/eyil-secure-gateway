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
#include <exception>
#include <linux/capability.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/syscall.h>

namespace Eyil
{
    namespace
    {
        // Used only by buffer filesystem operations on Gateway's private request
        // workers. Unlike setuid/setgid, these Linux calls change only this
        // thread's filesystem identity; effective/process credentials stay intact.
        class FilesystemIdentity
        {
        public:
            FilesystemIdentity(Diagnostics &log, uid_t uid, gid_t gid) noexcept
                : log(log)
                , originalUid(CurrentUid())
                , originalGid(CurrentGid())
            {
                // Linux can adjust effective capabilities when fsuid crosses
                // zero. Preserve those too, including deliberately disabled bits.
                if (!ReadCapabilities(originalCapabilities))
                    return;
                changed = true;
                setfsgid(gid);
                setfsuid(uid);
                active = CurrentUid() == uid && CurrentGid() == gid;
                if (!active)
                    errno = EPERM;
            }

            ~FilesystemIdentity() noexcept
            {
                const int error = errno;
                // The setters silently reject failures. Verify restoration, and
                // retry once before treating corrupted worker credentials as an
                // unrecoverable invariant violation. Never reuse a dirty worker.
                if (changed && !Restore() && !Restore())
                {
                    log.Error(Reason::Buffer, "filesystem_identity_restore_failed uid=%u gid=%u",
                        static_cast<unsigned>(originalUid), static_cast<unsigned>(originalGid));
                    std::terminate();
                }
                errno = error;
            }

            FilesystemIdentity(const FilesystemIdentity &) = delete;
            FilesystemIdentity &operator=(const FilesystemIdentity &) = delete;

            explicit operator bool() const noexcept { return active; }

        private:
            static uid_t CurrentUid() noexcept { return static_cast<uid_t>(setfsuid(static_cast<uid_t>(-1))); }

            static gid_t CurrentGid() noexcept { return static_cast<gid_t>(setfsgid(static_cast<gid_t>(-1))); }

            static bool ReadCapabilities(__user_cap_data_struct *data) noexcept
            {
                __user_cap_header_struct header { _LINUX_CAPABILITY_VERSION_3, 0 };
                return syscall(SYS_capget, &header, data) == 0;
            }

            bool Restore() noexcept
            {
                // Regain the original filesystem privileges before restoring GID.
                setfsuid(originalUid);
                setfsgid(originalGid);
                if (CurrentUid() != originalUid || CurrentGid() != originalGid)
                    return false;
                __user_cap_data_struct current[_LINUX_CAPABILITY_U32S_3] {};
                if (!ReadCapabilities(current))
                    return false;
                bool same = true;
                for (unsigned i = 0; i < _LINUX_CAPABILITY_U32S_3; ++i)
                    same = same && current[i].effective == originalCapabilities[i].effective
                        && current[i].permitted == originalCapabilities[i].permitted
                        && current[i].inheritable == originalCapabilities[i].inheritable;
                if (!same)
                {
                    __user_cap_header_struct header { _LINUX_CAPABILITY_VERSION_3, 0 };
                    if (syscall(SYS_capset, &header, originalCapabilities))
                        return false;
                    if (!ReadCapabilities(current))
                        return false;
                    for (unsigned i = 0; i < _LINUX_CAPABILITY_U32S_3; ++i)
                        if (current[i].effective != originalCapabilities[i].effective
                            || current[i].permitted != originalCapabilities[i].permitted
                            || current[i].inheritable != originalCapabilities[i].inheritable)
                            return false;
                }
                return true;
            }

            Diagnostics &log;
            uid_t originalUid;
            gid_t originalGid;
            __user_cap_data_struct originalCapabilities[_LINUX_CAPABILITY_U32S_3] {};
            bool changed = false;
            bool active = false;
        };
    }

    SharedBuffers::Lease::~Lease()
    {
        if (record)
            manager->Finish(record, uncertain, context);
    }

    SharedBuffers::Lease::Lease(Lease &&other) noexcept
        : manager(other.manager)
        , record(std::move(other.record))
        , uncertain(other.uncertain)
        , context(other.context)
    {
    }

    SharedBuffers::Lease &SharedBuffers::Lease::operator=(Lease &&other) noexcept
    {
        if (this != &other)
        {
            if (record)
                manager->Finish(record, uncertain, context);
            manager = other.manager;
            record = std::move(other.record);
            uncertain = other.uncertain;
            context = other.context;
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
        LogScope scope;
        scope.Context().connection = owner->peer.lifetime;
        scope.Context().uid = owner->peer.uid;
        scope.Context().service = RPC_SERVICE_CRYPTO;
        scope.Context().operation = CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER;
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
        record->gid = owner->peer.gid;
        record->size = size;
        record->id = Token();
        LogScope scope;
        scope.Context().Buffer(record->id);
        record->cleanupContext = scope.Context();
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
            {
                // Open procfs above with Gateway credentials. The captured FD
                // avoids another procfs traversal while using the peer identity.
                stage = "filesystem_identity";
                FilesystemIdentity identity(log, record->uid, record->gid);
                if (!identity)
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
            }
            // Credentials are restored before ownership checks, publication,
            // response construction or any subsequent ICrypto invocation.
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
        LogScope scope;
        scope.Context().connection = owner->peer.lifetime;
        scope.Context().uid = owner->peer.uid;
        scope.Context().Buffer(id);
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = records.find(id);
        if (it == records.end() || it->second->owner != owner->userId || !it->second->published || it->second->active
            || it->second->pending)
        {
            const char *reason = it == records.end() ? "unknown"
                : it->second->owner != owner->userId ? "owner"
                : !it->second->published             ? "unpublished"
                : it->second->pending                ? "release_pending"
                                                     : "busy";
            log.Error(Reason::Buffer, "buffer_unavailable cause=%s", reason);
            return CRYPTO_ERROR_INVALID_SESSION_STATE;
        }
        auto &record = it->second;
        for (size_t i = 0; i < count; ++i)
        {
            if (!Inside(regions[i], record->size))
            {
                log.Error(Reason::Buffer, "region_out_of_bounds region=%zu offset=%u length=%u capacity=%u", i,
                    regions[i].offset, regions[i].length, record->size);
                return CRYPTO_ERROR_INVALID_PARAMETERS;
            }
            for (size_t j = 0; j < i; ++j)
                if (Overlap(regions[i], regions[j]))
                {
                    log.Error(Reason::Buffer, "region_overlap first=%zu second=%zu", j, i);
                    return CRYPTO_ERROR_INVALID_PARAMETERS;
                }
        }
        lease.manager = this;
        lease.record = record;
        lease.context = scope.Context();
        record->active = true;
        return CRYPTO_ERROR_NONE;
    }

    CryptoError SharedBuffers::Release(const std::shared_ptr<Connection> &owner, const std::string &id)
    {
        LogScope scope;
        scope.Context().connection = owner->peer.lifetime;
        scope.Context().uid = owner->peer.uid;
        scope.Context().service = RPC_SERVICE_CRYPTO;
        scope.Context().operation = CRYPTO_OPERATION_RELEASE_SHARED_BUFFER;
        scope.Context().Buffer(id);
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
            record->cleanupContext = scope.Context();
        }
        Finalize(record);
        return CRYPTO_ERROR_NONE;
    }

    bool SharedBuffers::Finalize(const std::shared_ptr<Record> &record)
    {
        LogContext context;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!record->pending || record->active || record->gate || record->finalizing)
                return false;
            if (!records.count(record->id))
                return true;
            record->finalizing = true;
            context = record->cleanupContext;
        }
        LogScope scope(context);
        bool removed = !record->exists;
        int error = 0;
        if (!removed)
        {
            FilesystemIdentity identity(log, record->uid, record->gid);
            if (identity)
                removed = unlinkat(record->directory.Get(), record->name.c_str(), 0) == 0 || errno == ENOENT;
            error = errno;
        }
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

    void SharedBuffers::Finish(
        const std::shared_ptr<Record> &record, bool uncertain, const LogContext &context) noexcept
    {
        LogScope scope(context);
        try
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (uncertain)
                {
                    log.Warning(Reason::Buffer, "backend_access_uncertain uid=%u cleanup_deferred=1", record->uid);
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
                if (!record->pending)
                {
                    // Internal cleanup has no app release request to correlate.
                    record->cleanupContext.request = 0;
                    record->cleanupContext.operation = CRYPTO_OPERATION_RELEASE_SHARED_BUFFER;
                }
                record->pending = true;
                record->gate = false;
            }
            if (!Finalize(record))
                return false;
        }
    }
}
