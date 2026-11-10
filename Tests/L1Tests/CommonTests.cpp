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
#include "../../src/common/RuntimeSupport.h"
#include "TransportIdentity.h"
#include <fcntl.h>
#include <poll.h>
#include <set>
using namespace EyilTest;

namespace Metadata
{
    const char *lifetime = nullptr, *fd = nullptr;
    bool inject = false;
}

extern "C"
{
    const char *__real_zmq_msg_gets(const zmq_msg_t *, const char *);

    const char *__wrap_zmq_msg_gets(const zmq_msg_t *m, const char *key)
    {
        if (Metadata::inject)
        {
            if (std::strcmp(key, "Transport-Connection-Id") == 0)
                return Metadata::lifetime;
            if (std::strcmp(key, "Transport-FD") == 0)
                return Metadata::fd;
        }
        return __real_zmq_msg_gets(m, key);
    }
}

namespace
{
    void Descriptors()
    {
        int a = open("/dev/null", O_RDONLY | O_CLOEXEC), b = open("/dev/null", O_RDONLY | O_CLOEXEC);
        CHECK(a >= 0 && b >= 0);
        {
            Eyil::Fd x(a), y(b);
            y = std::move(x);
            CHECK(x.Get() == -1 && y.Get() == a);
            CHECK(fcntl(b, F_GETFD) == -1 && errno == EBADF);
            y = std::move(y);
            CHECK(y.Get() == a);
            Eyil::Fd z(std::move(y));
            CHECK(y.Get() == -1);
            CHECK(z.Release() == a);
            CHECK(z.Get() == -1);
        }
        CHECK(fcntl(a, F_GETFD) >= 0);
        close(a);
        int c = open("/dev/null", O_RDONLY);
        {
            Eyil::Fd d(c);
            d.Reset();
            CHECK(d.Get() == -1);
        }
        CHECK(fcntl(c, F_GETFD) == -1);
    }

    void Wakeups()
    {
        Eyil::Wakeup wake;
        pollfd p { wake.Get(), POLLIN, 0 };
        CHECK(poll(&p, 1, 0) == 0);
        CHECK(fcntl(wake.Get(), F_GETFD) & FD_CLOEXEC);
        CHECK(fcntl(wake.Get(), F_GETFL) & O_NONBLOCK);
        for (int i = 0; i < 100; ++i)
            wake.Signal();
        CHECK(poll(&p, 1, 0) == 1);
        wake.Drain();
        CHECK(poll(&p, 1, 0) == 0);
        wake.Drain();
        std::thread producer([&] { wake.Signal(); });
        CHECK(poll(&p, 1, 1000) == 1);
        producer.join();
        wake.Drain();
    }

    template <class T> T Option(void *socket, int option)
    {
        T value {};
        size_t size = sizeof(value);
        CHECK(zmq_getsockopt(socket, option, &value, &size) == 0);
        return value;
    }

    void Sockets()
    {
        Eyil::Context context;
        Eyil::Socket a(context.Get(), ZMQ_PAIR), b(context.Get(), ZMQ_PAIR);
        a.ConfigureTransport();
        CHECK(Option<int>(a.Get(), ZMQ_LINGER) == 0);
        for (int option : { ZMQ_HEARTBEAT_IVL, ZMQ_HEARTBEAT_TIMEOUT, ZMQ_HEARTBEAT_TTL })
            CHECK(Option<int>(a.Get(), option) == 0);
        CHECK(Option<int64_t>(a.Get(), ZMQ_MAXMSGSIZE) == -1);
        // Leave HWM at the linked libzmq defaults, compare with a fresh socket.
        for (int option : { ZMQ_SNDHWM, ZMQ_RCVHWM })
            CHECK(Option<int>(a.Get(), option) == Option<int>(b.Get(), option));
        a.Bind("inproc://common-unit");
        b.Connect("inproc://common-unit");
        CHECK(zmq_send(b.Get(), "a\0b", 3, ZMQ_SNDMORE) == 3);
        CHECK(zmq_send(b.Get(), "", 0, 0) == 0);
        Eyil::Message first, last;
        CHECK(first.Receive(a.Get(), 0) && first.More() && first.String() == std::string("a\0b", 3));
        CHECK(last.Receive(a.Get(), 0) && !last.More() && last.Size() == 0);
        CHECK(!last.Property("not-present"));
        CHECK(!last.Receive(a.Get()));
        CHECK(!a.Set(-123, 1) && a.Error() == EINVAL);
        CHECK(!a.Set(ZMQ_LINGER, 0) && a.Error() == EINVAL); // Preserve the first setup error.
        Eyil::Socket stringOption(context.Get(), ZMQ_PAIR), bind(context.Get(), ZMQ_PAIR);
        CHECK(!stringOption.SetString(-123, "x") && stringOption.Error() == EINVAL);
        CHECK(!bind.Bind("invalid://protocol") && bind.Error());
        CHECK(!b.Connect("invalid://protocol") && b.Error());
        CHECK(!last.Receive(nullptr) && last.Error() == ENOTSOCK);
        Eyil::Socket invalid(context.Get(), -100);
        CHECK(!invalid.Get() && invalid.Error() == EINVAL);
    }

    void Tokens()
    {
        std::set<std::string> seen;
        for (int i = 0; i < 500; ++i)
        {
            auto token = Eyil::Token();
            CHECK(token.size() == 32);
            CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
            CHECK(seen.insert(token).second);
        }
    }

    std::vector<std::string> lines;

    void Log(const char *s) { lines.emplace_back(s); }

    void ThrowLog(const char *) { throw std::bad_alloc(); }

    void Logs()
    {
        lines.clear();
        Eyil::Diagnostics log("test", Log);
        CHECK(log.Total(Eyil::Reason::Buffer) == 0);
        for (int i = 0; i < 1000; ++i)
            log.Error(Eyil::Reason::Buffer, "failure");
        CHECK(lines.size() == 1 && log.Total(Eyil::Reason::Buffer) == 1000);
        log.Error(Eyil::Reason::BackendCall, "other");
        CHECK(lines.size() == 2);
        clockAdvanceMs = 5001;
        log.Error(Eyil::Reason::Buffer, "next");
        clockAdvanceMs = 0;
        CHECK(lines.size() == 3 && lines.back().find("suppressed=999") != std::string::npos);
        log.Error(Eyil::Reason::Buffer, "last");
        log.Summary();
        CHECK(lines.size() == 4 && lines.back().find("total=1002") != std::string::npos);
        log.Summary();
        CHECK(lines.size() == 4);
        Eyil::Diagnostics broken("broken", ThrowLog);
        broken.Error(Eyil::Reason::Buffer, "x");
        broken.Error(Eyil::Reason::Buffer, "y");
        broken.Summary();
        CHECK(broken.Total(Eyil::Reason::Buffer) == 2);
    }

    void Decimals()
    {
        uint64_t value = 0;
        CHECK(Eyil::Decimal("0", value) && value == 0);
        CHECK(Eyil::Decimal("0007", value) && value == 7);
        CHECK(Eyil::Decimal("18446744073709551615", value) && value == UINT64_MAX);
        for (auto s :
            { static_cast<const char *>(nullptr), "", "-1", "+1", " 1", "1 ", "1x", "0x1", "18446744073709551616" })
            CHECK(!Eyil::Decimal(s, value));
        Eyil::Message empty;
        Eyil::Peer peer;
        CHECK(!Eyil::Credentials(empty, ":1:1:1", peer));
    }

    void Credentials()
    {
        struct Scope
        {
            Scope() { Metadata::inject = true; }

            ~Scope() { Metadata::inject = false; }
        } scope;

        Eyil::Message message;
        Metadata::lifetime = "42";
        Metadata::fd = "12";
        Eyil::Peer peer;
        CHECK(Eyil::Credentials(message, "host:1000:2000:3000", peer));
        CHECK(peer.lifetime == 42 && peer.fd == 12 && peer.uid == 1000 && peer.gid == 2000 && peer.pid == 3000);
        for (auto address : { "", ":1:2", ":1:2:0", ":-1:2:3", ":4294967296:2:3", ":1:4294967296:3", ":1:2:2147483648",
                 ":1::3", ":1:2:3x" })
            CHECK(!Eyil::Credentials(message, address, peer));
        for (auto value : { "0", "-1", "18446744073709551616" })
        {
            Metadata::lifetime = value;
            CHECK(!Eyil::Credentials(message, ":1:2:3", peer));
        }
        Metadata::lifetime = "1";
        for (auto value : { "-1", "2147483648", "bad" })
        {
            Metadata::fd = value;
            CHECK(!Eyil::Credentials(message, ":1:2:3", peer));
        }
    }

    std::string Number(uint64_t n) { return std::string(reinterpret_cast<char *>(&n), sizeof(n)); }

    void MonitorFrames()
    {
        auto decode = [](std::vector<std::string> frames, bool valid)
        {
            Eyil::Context context;
            Eyil::Socket a(context.Get(), ZMQ_PAIR), b(context.Get(), ZMQ_PAIR);
            a.Bind("inproc://monitor-unit");
            b.Connect("inproc://monitor-unit");
            Eyil::MonitorEvent event;
            CHECK(Eyil::ReadMonitor(a.Get(), event) == Eyil::MonitorRead::Empty);
            for (size_t i = 0; i < frames.size(); ++i)
                CHECK(zmq_send(b.Get(), frames[i].data(), frames[i].size(), i + 1 < frames.size() ? ZMQ_SNDMORE : 0)
                    >= 0);
            zmq_pollitem_t p { a.Get(), 0, ZMQ_POLLIN, 0 };
            CHECK(zmq_poll(&p, 1, 1000) == 1);
            if (valid)
            {
                CHECK(Eyil::ReadMonitor(a.Get(), event) == Eyil::MonitorRead::Event);
                CHECK(event.event == ZMQ_EVENT_DISCONNECTED);
                CHECK(event.values.size() == 2 && event.values[1] == 99);
            }
            else
                CHECK(Eyil::ReadMonitor(a.Get(), event) == Eyil::MonitorRead::Error);
        };
        const std::vector<std::string> good { Number(ZMQ_EVENT_DISCONNECTED), Number(2), Number(5), Number(99),
            "ipc://local", "" };
        decode(good, true);
        for (size_t n = 1; n < good.size(); ++n)
            decode({ good.begin(), good.begin() + n }, false);
        for (size_t i = 0; i < 4; ++i)
        {
            auto bad = good;
            bad[i] = "x";
            decode(bad, false);
        }
        auto bad = good;
        bad[1] = Number(6);
        decode(bad, false);
        bad = good;
        bad.push_back("extra");
        decode(bad, false);
    }
}

int main(int argc, char **argv)
{
    return RunCases(argc, argv,
        { { "descriptors", Descriptors }, { "wakeup", Wakeups }, { "sockets", Sockets }, { "tokens", Tokens },
            { "log-suppression", Logs }, { "identity-decimal", Decimals }, { "credentials", Credentials },
            { "monitor-framing", MonitorFrames } });
}
