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

#include "Faults.h"
#include <cstdlib>
#include <new>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace EyilTest
{
    thread_local Syscall failingCall = Syscall::None;
    thread_local int failureError = EIO, failuresLeft = 0, allocationCountdown = -1;
    thread_local bool simulateOwnership = false;
    std::atomic<long long> clockAdvanceMs { 0 };

    bool Fail(Syscall call)
    {
        if (failingCall == call && failuresLeft-- > 0)
        {
            errno = failureError;
            return true;
        }
        return false;
    }
}

void *operator new(std::size_t size)
{
    if (EyilTest::allocationCountdown >= 0 && EyilTest::allocationCountdown-- == 0)
        throw std::bad_alloc();
    if (void *p = std::calloc(1, size ? size : 1))
        return p;
    throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void *p) noexcept { std::free(p); }

void operator delete(void *p, std::size_t) noexcept { std::free(p); }

void operator delete[](void *p) noexcept { std::free(p); }

void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

// Keep nothrow allocation in the same test allocator domain. libzmq uses
// nothrow new; mixing ASan's default implementation with our delete/free
// overrides would report an allocator mismatch unrelated to the code tested.
void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    try
    {
        return ::operator new(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    try
    {
        return ::operator new[](size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }

void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }

extern "C"
{
    int __real_open(const char *, int, ...);
    int __real_openat(int, const char *, int, ...);
    int __real_ftruncate(int, off_t);
    int __real_fallocate(int, int, off_t, off_t);
    int __real_fchown(int, uid_t, gid_t);
    int __real_fchmod(int, mode_t);
    int __real_fstat(int, struct stat *);
    int __real_unlinkat(int, const char *, int);

    int __wrap_open(const char *path, int flags, ...)
    {
        if (EyilTest::Fail(EyilTest::Syscall::Open))
            return -1;
        mode_t mode = 0;
        if (flags & O_CREAT)
        {
            va_list args;
            va_start(args, flags);
            mode = va_arg(args, int);
            va_end(args);
        }
        return __real_open(path, flags, mode);
    }

    int __wrap_openat(int fd, const char *path, int flags, ...)
    {
        if (EyilTest::Fail(EyilTest::Syscall::OpenAt))
            return -1;
        mode_t mode = 0;
        if (flags & O_CREAT)
        {
            va_list args;
            va_start(args, flags);
            mode = va_arg(args, int);
            va_end(args);
        }
        return __real_openat(fd, path, flags, mode);
    }

    int __wrap_ftruncate(int fd, off_t size)
    {
        return EyilTest::Fail(EyilTest::Syscall::Truncate) ? -1 : __real_ftruncate(fd, size);
    }

    int __wrap_fallocate(int fd, int mode, off_t offset, off_t size)
    {
        return EyilTest::Fail(EyilTest::Syscall::Allocate) ? -1 : __real_fallocate(fd, mode, offset, size);
    }

    int __wrap_fchown(int fd, uid_t uid, gid_t gid)
    {
        if (EyilTest::Fail(EyilTest::Syscall::Chown))
            return -1;
        return EyilTest::simulateOwnership ? 0 : __real_fchown(fd, uid, gid);
    }

    int __wrap_fchmod(int fd, mode_t mode)
    {
        return EyilTest::Fail(EyilTest::Syscall::Chmod) ? -1 : __real_fchmod(fd, mode);
    }

    int __wrap_fstat(int fd, struct stat *status)
    {
        return EyilTest::Fail(EyilTest::Syscall::Stat) ? -1 : __real_fstat(fd, status);
    }

    int __wrap_unlinkat(int fd, const char *path, int flags)
    {
        return EyilTest::Fail(EyilTest::Syscall::Unlink) ? -1 : __real_unlinkat(fd, path, flags);
    }

    std::chrono::steady_clock::time_point __real__ZNSt6chrono3_V212steady_clock3nowEv() noexcept;

    std::chrono::steady_clock::time_point __wrap__ZNSt6chrono3_V212steady_clock3nowEv() noexcept
    {
        return __real__ZNSt6chrono3_V212steady_clock3nowEv()
            + std::chrono::milliseconds(EyilTest::clockAdvanceMs.load());
    }
}
