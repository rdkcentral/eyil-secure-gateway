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
#include <sys/stat.h>
using namespace EyilTest;

namespace
{
    struct Fixture
    {
        Eyil::Diagnostics log { "buffer-unit", IgnoreLog };
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

        // Only chown is simulated: this tests independent accounting keys without
        // needing root or assigning test files to a real unrelated account.
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
}

int main(int argc, char **argv)
{
    return RunCases(argc, argv,
        { { "sizes", BoundarySizes }, { "range-arithmetic", RangeArithmetic }, { "uid-isolation", UidIsolation },
            { "allocation-memory", AllocationMemory }, { "release-failure", ReleaseFailures },
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
