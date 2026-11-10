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

#include "TransportIdentity.h"
#include <charconv>
#include <climits>
#include <dlfcn.h>
#include <sys/stat.h>
#include <map>
#include <filesystem>

namespace Eyil
{
    bool Decimal(const char *text, uint64_t &value) noexcept
    {
        value = 0;
        if (!text || !*text)
            return false;
        const char *end = text + strlen(text);
        const auto parsed = std::from_chars(text, end, value);
        return parsed.ec == std::errc() && parsed.ptr == end;
    }

    bool Credentials(Message &message, const std::string &address, Peer &peer)
    {
        peer = {};
        uint64_t fd = 0;
        if (!Decimal(message.Property("Transport-Connection-Id"), peer.lifetime) || !peer.lifetime
            || !Decimal(message.Property("Transport-FD"), fd) || fd > INT_MAX)
            return false;
        peer.fd = static_cast<int>(fd);
        // Parse only the kernel-derived :uid:gid:host-pid suffix.
        size_t begin = address.size();
        for (unsigned i = 0; i < 3; ++i)
            if (!begin || (begin = address.rfind(':', begin - 1)) == std::string::npos)
                return false;
        uint64_t fields[3] {};
        for (unsigned i = 0; i < 3; ++i)
        {
            if (begin >= address.size() || address[begin] != ':')
                return false;
            const size_t end = address.find(':', ++begin);
            const char *first = address.data() + begin;
            const char *last = address.data() + (end == std::string::npos ? address.size() : end);
            const auto parsed = std::from_chars(first, last, fields[i]);
            if (parsed.ec != std::errc() || parsed.ptr != last)
                return false;
            begin = end;
        }
        if (begin != std::string::npos || fields[0] > UINT32_MAX || fields[1] > UINT32_MAX || !fields[2]
            || fields[2] > INT32_MAX)
            return false;
        peer.uid = fields[0];
        peer.gid = fields[1];
        peer.pid = fields[2];
        return true;
    }

    MonitorFunction RequireTransportPatch(void *router)
    {
        uint64_t zero = 0;
        if (zmq_setsockopt(router, ExpectedConnection, &zero, sizeof(zero)))
            return nullptr;
        return reinterpret_cast<MonitorFunction>(dlsym(RTLD_DEFAULT, "zmq_socket_monitor_versioned"));
    }

    MonitorRead ReadMonitor(void *socket, MonitorEvent &event)
    {
        Message first;
        if (!first.Receive(socket))
            return first.Error() ? MonitorRead::Error : MonitorRead::Empty;
        const auto number = [](Message &frame, uint64_t &value)
        {
            if (frame.Size() != sizeof(uint64_t))
                return false;
            memcpy(&value, frame.Data(), sizeof(value));
            return true;
        };
        if (!number(first, event.event) || !first.More())
            return MonitorRead::Error;
        Message count;
        uint64_t size = 0;
        if (!count.Receive(socket) || !number(count, size) || size > 5 || !count.More())
            return MonitorRead::Error;
        event.values.clear();
        for (uint64_t i = 0; i < size; ++i)
        {
            Message item;
            uint64_t value = 0;
            if (!item.Receive(socket) || !item.More() || !number(item, value))
                return MonitorRead::Error;
            event.values.push_back(value);
        }
        Message local, remote;
        if (!local.Receive(socket) || !local.More() || !remote.Receive(socket) || remote.More())
            return MonitorRead::Error;
        return MonitorRead::Event;
    }

    static TransportResult ProbeTransport(const std::string &temporaryDirectory)
    {
        // Production passes IShell::VolatilePath(); standalone tests use the OS temporary directory.
        std::error_code filesystemError;
        const auto base = temporaryDirectory.empty() ? std::filesystem::temp_directory_path(filesystemError)
                                                     : std::filesystem::path(temporaryDirectory);
        if (filesystemError)
            return { filesystemError.value(), "temporary directory unavailable" };
        std::filesystem::create_directories(base, filesystemError);
        if (filesystemError)
            return { filesystemError.value(), "cannot create transport test directory" };
        std::string directory = (base / "eyil-zmq-check.XXXXXX").string();
        std::string socketPath;
        if (!mkdtemp(directory.data()))
            return { errno, "cannot create private transport test directory" };

        struct Cleanup
        {
            const std::string &directory;
            const std::string &socketPath;

            ~Cleanup()
            {
                if (!socketPath.empty())
                    unlink(socketPath.c_str());
                rmdir(directory.c_str());
            }
        } cleanup { directory, socketPath };

        socketPath = directory + "/socket";

        Context context;
        if (context.Error())
            return { context.Error(), "probe context setup failed" };
        Socket zap(context.Get(), ZMQ_REP), router(context.Get(), ZMQ_ROUTER), monitor(context.Get(), ZMQ_PAIR);
        if (zap.Error() || router.Error() || monitor.Error())
            return { EIO, "probe socket setup failed" };
        auto versioned = RequireTransportPatch(router.Get());
        if (!versioned)
            return { ENOTSUP, "libzmq connection-lifetime patch missing (option 10001 or monitor v2)" };
        auto first = std::make_unique<Socket>(context.Get(), ZMQ_DEALER);
        auto second = std::make_unique<Socket>(context.Get(), ZMQ_DEALER);
        router.Set(ZMQ_PLAIN_SERVER, 1);
        router.Set(ZMQ_ROUTER_MANDATORY, 1);
        router.ConfigureTransport();
        if (router.Error())
            return { router.Error(), "probe ROUTER options failed" };
        if (versioned(router.Get(), "inproc://probe-monitor", ZMQ_EVENT_ACCEPTED | ZMQ_EVENT_DISCONNECTED, 2, ZMQ_PAIR))
            return { EPROTO, "versioned monitor setup failed" };

        // Stop the producer before any probe peer/collector is destroyed,
        // including on exceptions. A late disconnect sent to a closed monitor
        // pipe can leak the endpoint frame in libzmq 4.3.4.
        struct StopMonitor
        {
            void *socket;
            MonitorFunction function;

            ~StopMonitor() { function(socket, nullptr, 0, 2, ZMQ_PAIR); }
        } stopMonitor { router.Get(), versioned };

        monitor.Connect("inproc://probe-monitor");
        zap.Bind("inproc://zeromq.zap.01");
        const auto endpoint = "ipc://" + socketPath;
        router.Bind(endpoint.c_str());
        for (auto *socket : { first.get(), second.get() })
        {
            socket->ConfigureTransport();
            socket->SetString(ZMQ_PLAIN_USERNAME, "dummy");
            socket->SetString(ZMQ_PLAIN_PASSWORD, "dummy");
            socket->Connect(endpoint.c_str());
            if (socket->Error())
                return { socket->Error(), "probe DEALER setup failed" };
        }
        if (monitor.Error() || zap.Error() || router.Error())
            return { EIO, "probe bind/connect failed" };
        std::map<uint64_t, int> authenticated, accepted;
        std::vector<uint64_t> identities;
        bool requestSent = false, received = false, disconnected = false, replied = false;
        uint64_t requestIdentity = 0, closedIdentity = 0;
        std::vector<uint64_t> closed;
        const auto deadline = Clock::now() + CallTimeout;
        while (Clock::now() < deadline && !(received && disconnected && replied))
        {
            zmq_pollitem_t items[] = { { zap.Get(), 0, ZMQ_POLLIN, 0 }, { router.Get(), 0, ZMQ_POLLIN, 0 },
                { monitor.Get(), 0, ZMQ_POLLIN, 0 },
                { first->Get(), 0,
                    static_cast<short>(ZMQ_POLLIN | (identities.size() == 2 && !requestSent ? ZMQ_POLLOUT : 0)), 0 } };
            if (zmq_poll(items, 4, PollTimeout(deadline)) < 0 && errno != EINTR)
                return { EPROTO, "probe poll failed" };
            if (items[0].revents & ZMQ_POLLIN)
            {
                std::vector<std::string> frames;
                uint64_t lifetime = 0;
                for (;;)
                {
                    Message m;
                    if (!m.Receive(zap.Get()))
                        return { EPROTO, "probe ZAP framing" };
                    uint64_t id = 0;
                    if (!Decimal(m.Property("Transport-Connection-Id"), id) || !id || (lifetime && lifetime != id))
                        return { EPROTO, "probe ZAP identity" };
                    lifetime = id;
                    uint64_t fd = 0;
                    if (!Decimal(m.Property("Transport-FD"), fd) || fd > INT_MAX)
                        return { EPROTO, "probe FD range" };
                    if (frames.size() == 3)
                    {
                        Peer peer;
                        if (!Credentials(m, m.String(), peer) || peer.uid != getuid() || peer.gid != getgid()
                            || peer.pid != getpid())
                            return { EPROTO, "probe peer credentials" };
                        authenticated.emplace(id, static_cast<int>(fd));
                    }
                    frames.push_back(m.String());
                    if (!m.More())
                        break;
                }
                if (frames.size() != 8 || frames[5] != "PLAIN")
                    return { EPROTO, "probe ZAP mechanism" };
                identities.push_back(lifetime);
                const std::string parts[] = { "1.0", frames[1], "200", "OK", "probe", "" };
                for (unsigned i = 0; i < 6; ++i)
                    if (zmq_send(zap.Get(), parts[i].data(), parts[i].size(), i < 5 ? ZMQ_SNDMORE : 0) < 0)
                        return { EPROTO, "probe ZAP reply" };
            }
            if (identities.size() == 2 && !requestSent)
            {
                if (identities[0] == identities[1])
                    return { EPROTO, "reused transport lifetime" };
                second.reset();
                requestSent = zmq_send(first->Get(), "probe", 5, ZMQ_DONTWAIT) >= 0;
            }
            if (items[1].revents & ZMQ_POLLIN)
            {
                Message route, body;
                if (!route.Receive(router.Get()) || !route.More() || !body.Receive(router.Get()) || body.More())
                    return { EPROTO, "probe request framing" };
                uint64_t routeIdentity = 0;
                if (!Decimal(body.Property("Transport-Connection-Id"), requestIdentity)
                    || !Decimal(route.Property("Transport-Connection-Id"), routeIdentity)
                    || requestIdentity != routeIdentity || !body.Property("User-Id")
                    || std::string(body.Property("User-Id")) != "probe")
                    return { EPROTO, "probe request metadata" };
                uint64_t bodyFd = 0, routeFd = 0;
                if (!authenticated.count(requestIdentity) || !Decimal(body.Property("Transport-FD"), bodyFd)
                    || !Decimal(route.Property("Transport-FD"), routeFd)
                    || bodyFd != static_cast<uint64_t>(authenticated.at(requestIdentity)) || routeFd != bodyFd)
                    return { EPROTO, "probe request FD binding" };
                uint64_t wrong = UINT64_MAX;
                if (!router.Set(ExpectedConnection, wrong))
                    return { router.Error(), "probe route guard setup failed" };
                if (zmq_send(router.Get(), route.Data(), route.Size(), ZMQ_SNDMORE | ZMQ_DONTWAIT) >= 0
                    || errno != EHOSTUNREACH)
                    return { EPROTO, "probe stale route guard" };
                if (!router.Set(ExpectedConnection, requestIdentity))
                    return { router.Error(), "probe route guard setup failed" };
                if (zmq_send(router.Get(), route.Data(), route.Size(), ZMQ_SNDMORE | ZMQ_DONTWAIT) < 0
                    || zmq_send(router.Get(), "ok", 2, ZMQ_DONTWAIT) < 0)
                    return { EPROTO, "probe guarded reply" };
                received = true;
            }
            if (items[2].revents & ZMQ_POLLIN)
            {
                MonitorEvent event;
                const auto read = ReadMonitor(monitor.Get(), event);
                if (read == MonitorRead::Error)
                    return { EPROTO, "invalid probe monitor event" };
                if (read == MonitorRead::Event)
                {
                    if (event.event == ZMQ_EVENT_ACCEPTED)
                    {
                        if (event.values.size() != 2 || !event.values[1] || event.values[0] > INT_MAX)
                            return { EPROTO, "probe accept metadata" };
                        accepted.emplace(event.values[1], event.values[0]);
                    }
                    if (event.event == ZMQ_EVENT_DISCONNECTED)
                    {
                        if (event.values.size() != 5 || !event.values[4] || event.values[1] != getuid()
                            || event.values[2] != getgid() || event.values[3] != static_cast<uint64_t>(getpid()))
                            return { EPROTO, "probe disconnect metadata" };
                        if (!authenticated.count(event.values[4])
                            || event.values[0] != static_cast<uint64_t>(authenticated.at(event.values[4])))
                            return { EPROTO, "probe disconnect FD binding" };
                        closed.push_back(event.values[4]);
                        closedIdentity = event.values[4];
                    }
                }
            }
            if (received)
                for (auto id : closed)
                    if (id != requestIdentity && (id == identities[0] || id == identities[1]))
                        disconnected = true;
            if (items[3].revents & ZMQ_POLLIN)
            {
                Message reply;
                if (reply.Receive(first->Get()) && reply.String() == "ok")
                    replied = true;
            }
        }
        (void)closedIdentity;
        if (authenticated.size() != 2 || accepted != authenticated)
            return { EPROTO, "probe accept/ZAP binding mismatch" };
        if (!(received && disconnected && replied))
            return { EPROTO, "libzmq connection-lifetime self-test timed out" };
        return {};
    }

    TransportResult VerifyTransportPatch(const std::string &temporaryDirectory)
    {
        // STL/Protobuf allocations are external exception boundaries.
        try
        {
            return ProbeTransport(temporaryDirectory);
        }
        catch (const std::bad_alloc &)
        {
            return { ENOMEM, "transport self-test allocation failed" };
        }
        catch (...)
        {
            return { EIO, "transport self-test library failure" };
        }
    }
}
