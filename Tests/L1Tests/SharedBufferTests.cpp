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

#include "../support/Cases.h"
#include "../support/Faults.h"
#include "../support/Owners.h"
#include "SharedBuffers.h"
#include <filesystem>
#include <future>
#include <set>
#include <vector>
#include <sys/stat.h>
#include <sys/fsuid.h>
using namespace EyilTest;

namespace
{
    struct Fixture
    {
        explicit Fixture(Eyil::Diagnostics::Sink sink = IgnoreLog)
            : log("buffer-unit", sink)
        {
        }

        Eyil::Diagnostics log;
        Eyil::SharedBuffers buffers { log };
        std::shared_ptr<Eyil::Connection> a = Owner("a"), b = Owner("b");

        ~Fixture()
        {
            buffers.Cleanup(a->userId);
            buffers.Cleanup(b->userId);
        }
    };

    std::set<std::string> Objects()
    {
        std::set<std::string> names;
        for (const auto &entry : std::filesystem::directory_iterator("/dev/shm"))
            if (entry.path().filename().string().rfind("eyil.", 0) == 0)
                names.insert(entry.path().filename().string());
        return names;
    }

    void AllocationFailure(Syscall call, int error)
    {
        Fixture f;
        const auto before = Objects();
        Eyil::SharedBuffers::Allocation result;
        {
            FailSystemCall failure(call, error);
            result = f.buffers.Allocate(f.a, 4096);
        }
        CHECK(result.error == CRYPTO_ERROR_OPERATION_FAILED);
        CHECK(result.id.empty() && result.name.empty() && result.size == 0);
        CHECK(f.buffers.Charged(getuid()) == 0);
        CHECK(Objects() == before);
        auto recovered = f.buffers.Allocate(f.a, 4096);
        CHECK(recovered.error == CRYPTO_ERROR_NONE);
        CHECK(f.buffers.Release(f.a, recovered.id) == CRYPTO_ERROR_NONE);
        CHECK(f.buffers.Charged(getuid()) == 0);
    }

    void AllocationMemory()
    {
        Fixture f;
        const auto before = Objects();
        unsigned failed = 0;
        bool reachedSuccess = false;
        for (int index = 0; index < 128; ++index)
        {
            Eyil::SharedBuffers::Allocation allocation;
            {
                FailAllocation failure(index);
                allocation = f.buffers.Allocate(f.a, 4096);
            }
            if (allocation.error == CRYPTO_ERROR_NONE)
            {
                CHECK(f.buffers.Release(f.a, allocation.id) == CRYPTO_ERROR_NONE);
                reachedSuccess = true;
                break;
            }
            ++failed;
            CHECK(allocation.error == CRYPTO_ERROR_OPERATION_FAILED);
            CHECK(f.buffers.Charged(getuid()) == 0);
            CHECK(Objects() == before);
        }
        CHECK(failed > 5 && reachedSuccess);
        CHECK(Objects() == before);
    }

    void BoundarySizes()
    {
        Fixture f;
        CHECK(f.buffers.Allocate(f.a, 0).error == CRYPTO_ERROR_INVALID_PARAMETERS);
        CHECK(f.buffers.Allocate(f.a, UINT32_MAX).error == CRYPTO_ERROR_OVERSIZED_INPUT_DATA);
        for (uint32_t size : { 1u, 4095u, 4096u, Eyil::SharedBuffers::MaximumSize })
        {
            auto a = f.buffers.Allocate(f.a, size);
            CHECK(a.error == CRYPTO_ERROR_NONE && a.size == size);
            CHECK(a.name.rfind("/eyil.", 0) == 0);

            struct stat st
            {
            };

            CHECK(stat(("/dev/shm" + a.name).c_str(), &st) == 0);
            CHECK(st.st_size == size && st.st_uid == getuid() && (st.st_mode & 0777) == 0600);
            CHECK(f.buffers.Release(f.a, a.id) == CRYPTO_ERROR_NONE);
            CHECK(access(("/dev/shm" + a.name).c_str(), F_OK) != 0);
        }
    }

    void RangeArithmetic()
    {
        using B = Eyil::SharedBuffers;
        CHECK(B::Inside({ 0, 0 }, 0));
        CHECK(B::Inside({ UINT32_MAX, 0 }, UINT32_MAX));
        CHECK(B::Inside({ 0, UINT32_MAX }, UINT32_MAX));
        CHECK(!B::Inside({ UINT32_MAX, 1 }, UINT32_MAX));
        CHECK(!B::Inside({ UINT32_MAX - 1, 3 }, UINT32_MAX));
        CHECK(!B::Inside({ 5, 0 }, 4));
        CHECK(!B::Overlap({ 0, 0 }, { 0, 5 }));
        CHECK(!B::Overlap({ 0, 5 }, { 5, 5 }));
        CHECK(B::Overlap({ 4, 2 }, { 5, 2 }) && B::Overlap({ 5, 2 }, { 4, 2 }));
        CHECK(B::Overlap({ UINT32_MAX - 1, 2 }, { UINT32_MAX, 1 }));
    }

    void UidIsolation()
    {
        Fixture f;
        f.b->peer.uid = getuid() == UINT32_MAX ? getuid() - 1 : getuid() + 1;

        // Filesystem identities and chown are simulated: test independent quota
        // keys without root or assigning files to a real unrelated account.
        struct OwnershipScope
        {
            OwnershipScope() { simulateOwnership = true; }

            ~OwnershipScope() { simulateOwnership = false; }
        } scope;

        for (unsigned i = 0; i < 4; ++i)
        {
            CHECK(f.buffers.Allocate(f.a, 1024).error == CRYPTO_ERROR_NONE);
            CHECK(f.buffers.Allocate(f.b, 1024).error == CRYPTO_ERROR_NONE);
        }
        CHECK(f.buffers.Allocate(f.a, 1).error == CRYPTO_ERROR_OPERATION_FAILED);
        CHECK(f.buffers.Allocate(f.b, 1).error == CRYPTO_ERROR_OPERATION_FAILED);
        CHECK(f.buffers.Cleanup(f.a->userId));
        CHECK(f.buffers.Charged(f.a->peer.uid) == 0 && f.buffers.Charged(f.b->peer.uid) == 4);
    }

    struct MappedIdentity
    {
        MappedIdentity()
        {
            // Emulate foreign filesystem IDs without requiring root or changing
            // the test process identity. System-call wrappers observe each scope.
            simulateOwnership = true;
            simulateFilesystemIdentity = true;
            filesystemUid = geteuid();
            filesystemGid = getegid();
            createUid = unlinkUid = ownershipUid = directoryUid = static_cast<uid_t>(-1);
        }

        ~MappedIdentity()
        {
            simulateOwnership = false;
            simulateFilesystemIdentity = false;
        }
    };

    void CheckRestored()
    {
        CHECK(static_cast<uid_t>(setfsuid(-1)) == geteuid());
        CHECK(static_cast<gid_t>(setfsgid(-1)) == getegid());
    }

    void MappedAllocationFailure(Syscall call, int error)
    {
        MappedIdentity identity;
        Fixture f;
        f.a->peer.uid = getuid() + 101;
        f.a->peer.gid = getgid() + 101;
        const auto before = Objects();
        {
            FailSystemCall failure(call, error);
            const auto result = f.buffers.Allocate(f.a, 4096);
            CHECK(result.error == CRYPTO_ERROR_OPERATION_FAILED && result.id.empty());
        }
        CheckRestored();
        CHECK(f.buffers.Charged(f.a->peer.uid) == 0);
        CHECK(Objects() == before);
        const auto recovered = f.buffers.Allocate(f.a, 4096);
        CHECK(recovered.error == CRYPTO_ERROR_NONE);
        CHECK(f.buffers.Release(f.a, recovered.id) == CRYPTO_ERROR_NONE);
        CheckRestored();
    }

    void FilesystemScopes()
    {
        MappedIdentity identity;
        Fixture f;
        f.a->peer.uid = getuid() + 101;
        f.a->peer.gid = getgid() + 201;
        const auto allocation = f.buffers.Allocate(f.a, 4096);
        CHECK(allocation.error == CRYPTO_ERROR_NONE);
        CHECK(directoryUid == geteuid());
        CHECK(createUid == f.a->peer.uid && createGid == f.a->peer.gid);
        CHECK(ownershipUid == geteuid());
        CheckRestored();
        CHECK(f.buffers.Release(f.a, allocation.id) == CRYPTO_ERROR_NONE);
        CHECK(unlinkUid == f.a->peer.uid && unlinkGid == f.a->peer.gid);
        CHECK(f.buffers.Charged(f.a->peer.uid) == 0);
        CheckRestored();
    }

    void FilesystemFailures()
    {
        for (auto call : { Syscall::FilesystemUid, Syscall::FilesystemGid, Syscall::OpenAt, Syscall::Truncate,
                 Syscall::Allocate, Syscall::Chown, Syscall::Chmod, Syscall::Stat })
            MappedAllocationFailure(call, EACCES);
    }

    void FilesystemRestoreRetry()
    {
        for (auto call : { Syscall::RestoreFilesystemUid, Syscall::RestoreFilesystemGid })
        {
            MappedIdentity identity;
            Fixture f;
            f.a->peer.uid = getuid() + 101;
            f.a->peer.gid = getgid() + 201;
            Eyil::SharedBuffers::Allocation allocation;
            {
                FailSystemCall failure(call, EPERM);
                allocation = f.buffers.Allocate(f.a, 4096);
            }
            CHECK(allocation.error == CRYPTO_ERROR_NONE);
            CHECK(ownershipUid == geteuid());
            CheckRestored();
            CHECK(f.buffers.Release(f.a, allocation.id) == CRYPTO_ERROR_NONE);
            CheckRestored();
        }
    }

    void FilesystemCleanupFailure()
    {
        MappedIdentity identity;
        Fixture f;
        f.a->peer.uid = getuid() + 101;
        f.a->peer.gid = getgid() + 201;
        const auto allocation = f.buffers.Allocate(f.a, 4096);
        CHECK(allocation.error == CRYPTO_ERROR_NONE);
        {
            FailSystemCall failure(Syscall::FilesystemUid, EPERM);
            CHECK(f.buffers.Release(f.a, allocation.id) == CRYPTO_ERROR_NONE);
        }
        CheckRestored();
        CHECK(f.buffers.Charged(f.a->peer.uid) == 1);
        CHECK(access(("/dev/shm" + allocation.name).c_str(), F_OK) == 0);
        CHECK(f.buffers.Cleanup(f.a->userId));
        CHECK(f.buffers.Charged(f.a->peer.uid) == 0);
        CHECK(unlinkUid == f.a->peer.uid && unlinkGid == f.a->peer.gid);
        CheckRestored();
    }

    void FilesystemThreadIsolation()
    {
        Fixture f;
        auto run = [&](unsigned index)
        {
            MappedIdentity identity;
            auto owner = Owner(index ? "mapped-b" : "mapped-a", getuid() + 101 + index);
            owner->peer.gid = getgid() + 201 + index;
            for (unsigned i = 0; i < 20; ++i)
            {
                const auto allocation = f.buffers.Allocate(owner, 128);
                CHECK(allocation.error == CRYPTO_ERROR_NONE);
                CHECK(createUid == owner->peer.uid && createGid == owner->peer.gid);
                CheckRestored();
                CHECK(f.buffers.Release(owner, allocation.id) == CRYPTO_ERROR_NONE);
                CHECK(unlinkUid == owner->peer.uid && unlinkGid == owner->peer.gid);
                CheckRestored();
            }
        };
        const auto uid = setfsuid(-1), gid = setfsgid(-1);
        auto a = std::async(std::launch::async, run, 0);
        auto b = std::async(std::launch::async, run, 1);
        a.get();
        b.get();
        CHECK(setfsuid(-1) == uid && setfsgid(-1) == gid);
        CHECK(geteuid() == getuid() && getegid() == getgid());
    }

    void FilesystemSavedIdentity()
    {
        MappedIdentity identity;
        Fixture f;
        filesystemUid = geteuid() + 301;
        filesystemGid = getegid() + 401;
        const auto savedUid = filesystemUid;
        const auto savedGid = filesystemGid;
        f.a->peer.uid = getuid() + 101;
        f.a->peer.gid = getgid() + 201;
        const auto allocation = f.buffers.Allocate(f.a, 4096);
        CHECK(allocation.error == CRYPTO_ERROR_NONE);
        CHECK(directoryUid == savedUid && ownershipUid == savedUid);
        CHECK(filesystemUid == savedUid && filesystemGid == savedGid);
        CHECK(f.buffers.Release(f.a, allocation.id) == CRYPTO_ERROR_NONE);
        CHECK(filesystemUid == savedUid && filesystemGid == savedGid);
    }

    void FilesystemDeferredCleanup()
    {
        MappedIdentity identity;
        Fixture f;
        f.a->peer.uid = getuid() + 101;
        f.a->peer.gid = getgid() + 201;
        const auto capturedGid = f.a->peer.gid;
        const auto allocation = f.buffers.Allocate(f.a, 4096);
        CHECK(allocation.error == CRYPTO_ERROR_NONE);
        Eyil::SharedBuffers::Lease lease;
        const Eyil::SharedBuffers::Region region { 0, 16 };
        CHECK(f.buffers.TryUse(f.a, allocation.id, &region, 1, lease) == CRYPTO_ERROR_NONE);
        CHECK(f.buffers.Release(f.a, allocation.id) == CRYPTO_ERROR_NONE);
        CHECK(unlinkUid == static_cast<uid_t>(-1));
        f.a->closing = true;
        f.buffers.Gate(f.a->userId);
        f.a->peer.gid += 1; // Cleanup uses the captured allocation identity.
        lease = {};
        CHECK(f.buffers.Charged(f.a->peer.uid) == 1);
        CHECK(f.buffers.Cleanup(f.a->userId));
        CHECK(unlinkUid == f.a->peer.uid && unlinkGid == capturedGid);
        CHECK(f.buffers.Charged(f.a->peer.uid) == 0);
        CheckRestored();
    }

    void ReleaseFailures()
    {
        Fixture f;
        auto a = f.buffers.Allocate(f.a, 4096);
        CHECK(a.error == CRYPTO_ERROR_NONE);
        {
            FailSystemCall failure(Syscall::Unlink, EACCES);
            CHECK(f.buffers.Release(f.a, a.id) == CRYPTO_ERROR_NONE);
        }
        CHECK(f.buffers.Charged(getuid()) == 1);
        CHECK(access(("/dev/shm" + a.name).c_str(), F_OK) == 0);
        CHECK(f.buffers.Release(f.a, a.id) == CRYPTO_ERROR_INVALID_SESSION_STATE);
        CHECK(f.buffers.Cleanup(f.a->userId));
        CHECK(f.buffers.Charged(getuid()) == 0);
        CHECK(f.buffers.Cleanup(f.a->userId));
        auto b = f.buffers.Allocate(f.b, 4096);
        CHECK(unlink(("/dev/shm" + b.name).c_str()) == 0);
        CHECK(f.buffers.Release(f.b, b.id) == CRYPTO_ERROR_NONE);
        CHECK(f.buffers.Charged(getuid()) == 0);
    }

    void Leases()
    {
        Fixture f;
        auto a = f.buffers.Allocate(f.a, 4096);
        Eyil::SharedBuffers::Lease first, second;
        const Eyil::SharedBuffers::Region input { 0, 4096 };
        CHECK(f.buffers.TryUse(f.b, a.id, &input, 1, first) == CRYPTO_ERROR_INVALID_SESSION_STATE);
        CHECK(f.buffers.TryUse(f.a, a.id, &input, 1, first) == CRYPTO_ERROR_NONE);
        CHECK(first.Capacity() == 4096 && !first.Path().empty());
        second = std::move(first);
        CHECK(!first && second);
        Eyil::SharedBuffers::Lease third(std::move(second));
        CHECK(!second && third);
        f.buffers.Gate(f.a->userId);
        CHECK(f.buffers.Release(f.a, a.id) == CRYPTO_ERROR_NONE);
        CHECK(f.buffers.Charged(getuid()) == 1);
        CHECK(!f.buffers.Cleanup(f.a->userId));
        CHECK(f.buffers.Charged(getuid()) == 1);
        third = {};
        CHECK(f.buffers.Cleanup(f.a->userId));
        CHECK(f.buffers.Charged(getuid()) == 0);
        f.a->closing = true;
        CHECK(f.buffers.Allocate(f.a, 4096).error == CRYPTO_ERROR_OPERATION_FAILED);
        CHECK(f.buffers.Charged(getuid()) == 0);
    }

    void ReleaseRace()
    {
        for (int round = 0; round < 20; ++round)
        {
            Fixture f;
            const auto a = f.buffers.Allocate(f.a, 4096);
            Eyil::SharedBuffers::Lease lease;
            const Eyil::SharedBuffers::Region input { 0, 32 };
            CHECK(f.buffers.TryUse(f.a, a.id, &input, 1, lease) == CRYPTO_ERROR_NONE);
            auto release = std::async(std::launch::async, [&] { return f.buffers.Release(f.a, a.id); });
            auto cleanup = std::async(std::launch::async,
                [&]
                {
                    f.a->closing = true;
                    return f.buffers.Cleanup(f.a->userId);
                });
            lease = {};
            auto result = release.get();
            CHECK(result == CRYPTO_ERROR_NONE || result == CRYPTO_ERROR_INVALID_SESSION_STATE);
            cleanup.get();
            CHECK(f.buffers.Cleanup(f.a->userId));
            CHECK(f.buffers.Charged(getuid()) == 0);
        }
    }

    std::vector<std::string> bufferLogs;

    void CaptureBufferLog(const char *text) { bufferLogs.emplace_back(text); }

    void LogCorrelation()
    {
        bufferLogs.clear();
        Fixture f(CaptureBufferLog);
        f.a->peer.lifetime = 567;
        Eyil::SharedBuffers::Allocation allocation;
        {
            Eyil::LogScope scope({ 567, 123, getuid(), RPC_SERVICE_CRYPTO });
            allocation = f.buffers.Allocate(f.a, 4096);
        }
        CHECK(allocation.error == CRYPTO_ERROR_NONE);
        Eyil::SharedBuffers::Lease lease;
        const Eyil::SharedBuffers::Region region { 0, 16 };
        {
            Eyil::LogScope scope({ 567, 124, getuid(), RPC_SERVICE_CRYPTO, CRYPTO_OPERATION_ENCRYPT });
            CHECK(f.buffers.TryUse(f.a, allocation.id, &region, 1, lease) == CRYPTO_ERROR_NONE);
        }
        {
            Eyil::LogScope scope({ 567, 125, getuid(), RPC_SERVICE_CRYPTO });
            CHECK(f.buffers.Release(f.a, allocation.id) == CRYPTO_ERROR_NONE);
        }
        {
            FailSystemCall failure(Syscall::Unlink, EACCES);
            lease = {}; // Deferred cleanup retains the release request's context.
        }
        CHECK(bufferLogs.size() == 1);
        const auto &line = bufferLogs.front();
        CHECK(line.find("buffer_cleanup_failed") != std::string::npos);
        CHECK(line.find("connection=567") != std::string::npos);
        CHECK(line.find("request_id=125 service=1") != std::string::npos);
        CHECK(line.find("buffer_id=" + allocation.id) != std::string::npos);
        CHECK(line.find("errno=13") != std::string::npos);
        CHECK(f.buffers.Cleanup(f.a->userId));
    }

}

int main(int argc, char **argv)
{
    return RunCases(argc, argv,
        { { "log-correlation", LogCorrelation }, { "sizes", BoundarySizes }, { "range-arithmetic", RangeArithmetic },
            { "uid-isolation", UidIsolation }, { "allocation-memory", AllocationMemory },
            { "filesystem-scopes", FilesystemScopes }, { "filesystem-failures", FilesystemFailures },
            { "filesystem-restore-retry", FilesystemRestoreRetry },
            { "filesystem-cleanup-failure", FilesystemCleanupFailure },
            { "filesystem-thread-isolation", FilesystemThreadIsolation },
            { "filesystem-saved-identity", FilesystemSavedIdentity },
            { "filesystem-deferred-cleanup", FilesystemDeferredCleanup }, { "release-failure", ReleaseFailures },
            { "lease-lifetime", Leases }, { "release-race", ReleaseRace },
            { "open-denied", [] { AllocationFailure(Syscall::Open, EACCES); } },
            { "exclusive-create-failed", [] { AllocationFailure(Syscall::OpenAt, EEXIST); } },
            { "truncate-failed", [] { AllocationFailure(Syscall::Truncate, EIO); } },
            { "allocate-enospc", [] { AllocationFailure(Syscall::Allocate, ENOSPC); } },
            { "chown-failed", [] { AllocationFailure(Syscall::Chown, EPERM); } },
            { "chmod-failed", [] { AllocationFailure(Syscall::Chmod, EPERM); } },
            { "stat-failed", [] { AllocationFailure(Syscall::Stat, EIO); } },
            { "allocate-eintr", []
                {
                    Fixture f;
                    FailSystemCall failure(Syscall::Allocate, EINTR, 3);
                    CHECK(f.buffers.Allocate(f.a, 4096).error == CRYPTO_ERROR_NONE);
                } } });
}
