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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <unistd.h>
#include <zmq.h>

namespace Eyil
{
    using Clock = std::chrono::steady_clock;
    constexpr auto CallTimeout = std::chrono::milliseconds(15000);
    constexpr auto WorkerTimeout = std::chrono::milliseconds(10000);
    constexpr const char *Endpoint = "ipc:///tmp/app-eyil-gateway.ipc";

    inline long PollTimeout(Clock::time_point deadline, Clock::time_point now = Clock::now()) noexcept
    {
        if (deadline == Clock::time_point::max())
            return -1;
        if (deadline <= now)
            return 0;
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        return static_cast<long>(std::min<int64_t>(remaining, std::numeric_limits<long>::max()));
    }

    // Copied with retained work; no payload, app identity string or heap allocation.
    struct LogContext
    {
        uint64_t connection = 0;
        uint64_t request = 0;
        uint32_t uid = 0;
        int service = 0;
        int operation = 0;
        std::array<char, 65> buffer {};

        void Buffer(const std::string &id) noexcept
        {
            buffer.fill(0);
            for (size_t i = 0; i < id.size() && i + 1 < buffer.size(); ++i)
            {
                const char c = id[i];
                buffer[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
                                || c == '_' || c == '.')
                    ? c
                    : '?';
            }
        }
    };

    // Nested synchronous calls inherit correlation; another worker has its own scope.
    class LogScope
    {
    public:
        explicit LogScope(LogContext value = Current()) noexcept
            : context(value)
            , previous(active)
        {
            active = &context;
        }

        ~LogScope() { active = previous; }

        LogScope(const LogScope &) = delete;
        LogScope &operator=(const LogScope &) = delete;

        LogContext &Context() noexcept { return context; }

        static LogContext Current() noexcept { return active ? *active : LogContext {}; }

    private:
        LogContext context;
        const LogContext *previous;
        inline static thread_local const LogContext *active = nullptr;
    };

    class Fd
    {
    public:
        explicit Fd(int fd = -1) noexcept
            : value(fd)
        {
        }

        ~Fd() { Reset(); }

        Fd(const Fd &) = delete;
        Fd &operator=(const Fd &) = delete;

        Fd(Fd &&other) noexcept
            : value(other.Release())
        {
        }

        Fd &operator=(Fd &&other) noexcept
        {
            if (this != &other)
                Reset(other.Release());
            return *this;
        }

        int Get() const noexcept { return value; }

        int Release() noexcept
        {
            const int result = value;
            value = -1;
            return result;
        }

        void Reset(int fd = -1) noexcept
        {
            if (value >= 0)
                ::close(value);
            value = fd;
        }

    private:
        int value;
    };

    class Wakeup
    {
    public:
        Wakeup()
            : fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
        {
            error = fd.Get() < 0 ? errno : 0;
        }

        int Get() const noexcept { return fd.Get(); }

        int Error() const noexcept { return error; }

        void Signal() noexcept
        {
            const uint64_t one = 1;
            while (write(fd.Get(), &one, sizeof(one)) < 0 && errno == EINTR)
            {
            }
        }

        void Drain() noexcept
        {
            uint64_t value;
            while (read(fd.Get(), &value, sizeof(value)) >= 0 || errno == EINTR)
            {
            }
        }

    private:
        Fd fd;
        int error = 0;
    };

    class Context
    {
    public:
        Context()
            : value(zmq_ctx_new())
        {
            error = value ? 0 : errno;
        }

        ~Context()
        {
            if (value)
                while (zmq_ctx_term(value) < 0 && errno == EINTR)
                {
                }
        }

        Context(const Context &) = delete;
        Context &operator=(const Context &) = delete;

        void *Get() const noexcept { return value; }

        int Error() const noexcept { return error; }

    private:
        void *value;
        int error = 0;
    };

    class Socket
    {
    public:
        Socket(void *context, int type)
            : value(zmq_socket(context, type))
        {
            if (!value)
            {
                error = errno;
                return;
            }
            const int linger = 0;
            if (!Set(ZMQ_LINGER, linger))
            {
                zmq_close(value);
                value = nullptr;
            }
        }

        ~Socket()
        {
            if (value)
                zmq_close(value);
        }

        Socket(const Socket &) = delete;
        Socket &operator=(const Socket &) = delete;

        void *Get() const noexcept { return value; }

        int Error() const noexcept { return error; }

        template <class T> bool Set(int option, const T &data) noexcept
        {
            return Apply(error ? -1 : zmq_setsockopt(value, option, &data, sizeof(data)));
        }

        bool SetString(int option, const std::string &data) noexcept
        {
            return Apply(error ? -1 : zmq_setsockopt(value, option, data.data(), data.size()));
        }

        bool Bind(const char *endpoint) noexcept { return Apply(error ? -1 : zmq_bind(value, endpoint)); }

        bool Connect(const char *endpoint) noexcept { return Apply(error ? -1 : zmq_connect(value, endpoint)); }

        bool ConfigureTransport() noexcept
        {
            return Set(ZMQ_HEARTBEAT_IVL, 0) && Set(ZMQ_HEARTBEAT_TIMEOUT, 0) && Set(ZMQ_HEARTBEAT_TTL, 0)
                && Set(ZMQ_MAXMSGSIZE, int64_t { -1 });
        }

    private:
        bool Apply(int result) noexcept
        {
            if (result != 0 && !error)
                error = errno;
            return error == 0;
        }

        void *value;
        int error = 0;
    };

    class Message
    {
    public:
        Message() noexcept
        {
            initialized = zmq_msg_init(&value) == 0;
            if (!initialized)
                error = errno;
        }

        ~Message()
        {
            if (initialized)
                zmq_msg_close(&value);
        }

        Message(const Message &) = delete;
        Message &operator=(const Message &) = delete;

        bool Receive(void *socket, int flags = ZMQ_DONTWAIT) noexcept
        {
            if (!initialized)
                return false;
            error = 0;
            if (zmq_msg_recv(&value, socket, flags) >= 0)
                return true;
            if (errno != EAGAIN && errno != EINTR)
                error = errno;
            return false;
        }

        int Error() const noexcept { return error; }

        const void *Data() noexcept { return initialized ? zmq_msg_data(&value) : nullptr; }

        size_t Size() const noexcept { return initialized ? zmq_msg_size(const_cast<zmq_msg_t *>(&value)) : 0; }

        bool More() const noexcept { return initialized && zmq_msg_more(&value) != 0; }

        std::string String()
        {
            return initialized ? std::string(static_cast<const char *>(Data()), Size()) : std::string {};
        }

        const char *Property(const char *name) { return initialized ? zmq_msg_gets(&value, name) : nullptr; }

    private:
        zmq_msg_t value;
        int error = 0;
        bool initialized = false;
    };

    inline std::string Token()
    {
        std::array<unsigned char, 16> bytes {};
        size_t done = 0;
        while (done < bytes.size())
        {
            const auto n = getrandom(bytes.data() + done, bytes.size() - done, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
            {
                if (n == 0)
                    errno = EIO;
                return {};
            }
            done += static_cast<size_t>(n);
        }
        constexpr char hex[] = "0123456789abcdef";
        std::string result(bytes.size() * 2, '0');
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            result[2 * i] = hex[bytes[i] >> 4];
            result[2 * i + 1] = hex[bytes[i] & 15];
        }
        return result;
    }

    // Fixed categories keep error suppression bounded even for arbitrary peer input.
    enum class Reason
    {
        Transport,
        Protocol,
        Allocation,
        BackendLookup,
        BackendCall,
        Authentication,
        Buffer,
        WorkerExhausted,
        WorkerWaiting,
        DroppedReply,
        Callback,
        Setup,
        Count
    };

    class Diagnostics
    {
    public:
        using Sink = void (*)(const char *);

        explicit Diagnostics(const char *component, Sink sink = nullptr)
            : component(component)
            , sink(sink)
        {
        }

        void Error(Reason reason, const char *format, ...) noexcept
        {
            va_list args;
            va_start(args, format);
            Write(0, reason, format, args);
            va_end(args);
        }

        void Warning(Reason reason, const char *format, ...) noexcept
        {
            va_list args;
            va_start(args, format);
            Write(1, reason, format, args);
            va_end(args);
        }

        uint64_t Total(Reason reason) const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return buckets[0][static_cast<size_t>(reason)].total + buckets[1][static_cast<size_t>(reason)].total;
        }

        void Summary() noexcept
        {
            try
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (unsigned level = 0; level < buckets.size(); ++level)
                    for (size_t i = 0; i < buckets[level].size(); ++i)
                    {
                        auto &b = buckets[level][i];
                        if (!b.suppressed)
                            continue;
                        char text[512];
                        snprintf(text, sizeof(text),
                            "%s %s category=%zu suppressed=%llu total=%llu last_connection=%llu last_request_id=%llu\n",
                            component, Level(level), i, static_cast<unsigned long long>(b.suppressed),
                            static_cast<unsigned long long>(b.total),
                            static_cast<unsigned long long>(b.context.connection),
                            static_cast<unsigned long long>(b.context.request));
                        Emit(text);
                        b.suppressed = 0;
                    }
            }
            catch (...)
            {
            }
        }

    private:
        static const char *Level(unsigned level) { return level ? "WARNING" : "ERROR"; }

        void Emit(const char *text)
        {
            if (sink)
                sink(text);
            else
                fputs(text, stderr);
        }

        void Write(unsigned level, Reason reason, const char *format, va_list args) noexcept
        {
            try
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto &bucket = buckets[level][static_cast<size_t>(reason)];
                ++bucket.total;
                bucket.context = LogScope::Current();
                const auto now = Clock::now();
                if (bucket.total != 1 && now - bucket.last < std::chrono::seconds(5))
                {
                    ++bucket.suppressed;
                    return;
                }
                char detail[512];
                vsnprintf(detail, sizeof(detail), format, args);
                char fields[320] {};
                size_t used = 0;
                const auto append = [&](const char *key, unsigned long long value)
                { used += snprintf(fields + used, sizeof(fields) - used, " %s=%llu", key, value); };
                const auto &context = bucket.context;
                if (context.connection)
                {
                    append("connection", context.connection);
                    append("uid", context.uid);
                }
                if (context.request)
                {
                    append("request_id", context.request);
                    append("service", context.service);
                }
                if (context.operation)
                    append("operation", context.operation);
                if (context.buffer[0])
                    snprintf(fields + used, sizeof(fields) - used, " buffer_id=%s", context.buffer.data());
                char output[1024];
                snprintf(output, sizeof(output), "%s %s category=%u %s%s suppressed=%llu\n", component, Level(level),
                    static_cast<unsigned>(reason), detail, fields, static_cast<unsigned long long>(bucket.suppressed));
                bucket.suppressed = 0;
                bucket.last = now;
                Emit(output);
            }
            catch (...)
            {
            }
        }

        struct Bucket
        {
            Clock::time_point last {};
            uint64_t total = 0;
            uint64_t suppressed = 0;
            LogContext context;
        };

        const char *component;
        Sink sink;
        mutable std::mutex mutex;
        std::array<std::array<Bucket, static_cast<size_t>(Reason::Count)>, 2> buckets {};
    };
}
