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

#include "GatewayRuntime.h"
#include <ProtocolVersion.h>
#include <climits>
#include <sys/stat.h>

namespace Eyil
{
    namespace Core = WPEFramework::Core;

    GatewayRuntime::GatewayRuntime(CryptoServiceAdapter::Resolver resolver, unsigned count, std::string address,
        Diagnostics::Sink sink, std::string temporaryPath, FailureHandler failureCallback)
        : log("EyilGateway", sink)
        , adapter(std::move(resolver), buffers, log)
        , failureHandler(std::move(failureCallback))
        , workers(count)
        , endpoint(std::move(address))
        , temporaryDirectory(std::move(temporaryPath))
    {
    }

    GatewayRuntime::~GatewayRuntime() { StopAndWait(); }

    bool GatewayRuntime::Start()
    {
        std::lock_guard<std::mutex> serial(stopMutex);
        if (started || !workers || workers > UINT8_MAX || wake.Error() || zapWake.Error() || monitorWake.Error())
        {
            log.Error(Reason::Setup, "invalid runtime startup started=%u workers=%u wake_errno=%d", started, workers,
                wake.Error() ? wake.Error() : (zapWake.Error() ? zapWake.Error() : monitorWake.Error()));
            return false;
        }
        const auto probe = VerifyTransportPatch(temporaryDirectory);
        if (!probe)
        {
            log.Error(Reason::Setup, "%s error=%d", probe.message, probe.error);
            return false;
        }
        // Allocation and thread creation are external library exception boundaries.
        try
        {
            context = std::make_unique<Context>();
            if (context->Error())
            {
                log.Error(Reason::Setup, "context creation failed errno=%d", context->Error());
                context.reset();
                return false;
            }
            pool = std::make_unique<Core::ThreadPool>(
                workers, Core::Thread::DefaultStackSize(), workers, &dispatcher, nullptr, nullptr, nullptr);
            pool->Run();
            started = true;
            zap = std::thread(&GatewayRuntime::Authenticate, this);
            monitor = std::thread(&GatewayRuntime::Monitor, this);
            io = std::thread(&GatewayRuntime::Io, this);
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return listenerReady || failed; });
            if (!failed)
            {
                notifyFailures = true;
                return true;
            }
        }
        catch (const std::exception &error)
        {
            log.Error(Reason::Setup, "runtime construction failed: %s", error.what());
        }
        catch (...)
        {
            log.Error(Reason::Setup, "runtime construction failed");
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            auxiliaryStop = true;
            zapWake.Signal();
            monitorWake.Signal();
            StateChanged();
        }
        wake.Signal();
        if (io.joinable())
            io.join();
        if (zap.joinable())
            zap.join();
        if (monitor.joinable())
            monitor.join();
        if (pool)
            pool->Stop();
        pool.reset();
        context.reset();
        started = false;
        return false;
    }

    void GatewayRuntime::StopAndWait()
    {
        std::lock_guard<std::mutex> serial(stopMutex);
        if (!started)
            return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            notifyFailures = false;
            for (const auto &entry : connections)
                Close(entry.second, false);
            StateChanged();
        }
        wake.Signal();
        if (io.joinable())
            io.join();
        {
            std::lock_guard<std::mutex> lock(mutex);
            auxiliaryStop = true;
            zapWake.Signal();
            monitorWake.Signal();
            StateChanged();
        }
        if (zap.joinable())
            zap.join();
        if (monitor.joinable())
            monitor.join();
        pool->Stop();
        pool.reset();
        context.reset();
        started = false;
        log.Summary();
    }

    void GatewayRuntime::Fatal(const char *reason) noexcept
    {
        log.Error(Reason::Transport, "runtime_failure reason=%s", reason);
        bool notify;
        {
            std::lock_guard<std::mutex> lock(mutex);
            notify = notifyFailures && !failed && !stopping;
            notifyFailures = false;
            failed = true;
            stopping = true;
            for (const auto &entry : connections)
                Close(entry.second, false);
            StateChanged();
        }
        wake.Signal();
        if (notify && failureHandler)
        {
            // The callback only schedules framework work. It must not destroy
            // the runtime on a socket thread that its destructor has to join.
            try
            {
                failureHandler();
            }
            catch (...)
            {
                log.Error(Reason::Callback, "runtime deactivation scheduling failed");
            }
        }
    }

    void GatewayRuntime::Close(const std::shared_ptr<Connection> &connection, bool disconnected)
    {
        const bool routeLost = disconnected && !connection->disconnected;
        connection->disconnected = connection->disconnected || disconnected;
        const bool closing = !connection->closing.exchange(true);
        if (closing)
            buffers.Gate(connection->userId);
        if (closing || routeLost)
            StateChanged();
    }

    void GatewayRuntime::StateChanged()
    {
        ++stateRevision;
        changed.notify_all();
        wake.Signal();
    }

    void GatewayRuntime::LogWorkerWait(const Work &work, bool expired)
    {
        LogScope scope({ work.connection->peer.lifetime, work.request.request_id(), work.connection->peer.uid,
            work.request.service() });
        const auto elapsed
            = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - work.admitted).count();
        const char *format = "reason=%s workers=%u running=%u reserved=%u lifecycle_running=%u wait_ms=%lld";
        if (expired)
            log.Error(Reason::WorkerExhausted, format, "worker_pool_exhausted", workers, running, reserved,
                lifecycleRunning, static_cast<long long>(elapsed));
        else
            log.Warning(Reason::WorkerWaiting, format, "worker_pool_wait", workers, running, reserved, lifecycleRunning,
                static_cast<long long>(elapsed));
    }

    bool GatewayRuntime::Schedule(const std::shared_ptr<Work> &work)
    {
        if (reserved + running >= workers)
            return false;
        // Every Submit has a reserved worker. At most workers jobs can be held by
        // Thunder, whose internal queue has workers entries; Insert(0) cannot fill.
        auto job = Core::ProxyType<Job>::Create(this, work);
        ++reserved;
        pool->Submit(Core::ProxyType<Core::IDispatch>(job), 0);
        return true;
    }

    void GatewayRuntime::Respond(Work &work, RpcStatus status, const std::string &payload) noexcept
    {
        const auto prepare = [&](RpcStatus result, const std::string &body) noexcept
        {
            try
            {
                work.response.clear();
                RpcResponse response;
                response.set_request_id(work.request.request_id());
                response.set_service(work.request.service());
                response.set_schema_version(EYIL_RPC_SCHEMA_VERSION);
                response.set_status(result);
                if (!body.empty())
                    response.set_payload(body);
                if (response.SerializeToString(&work.response))
                    return true;
            }
            catch (const std::bad_alloc &)
            {
                log.Error(Reason::Allocation, "response_allocation_failed request_id=%llu status=%d",
                    static_cast<unsigned long long>(work.request.request_id()), result);
                work.response.clear();
                return false;
            }
            catch (...)
            {
                // Only response construction/serialization is guarded here.
                // Transport failures retain their separate socket-lifetime policy.
            }
            log.Error(Reason::Protocol, "response_serialization_failed request_id=%llu status=%d",
                static_cast<unsigned long long>(work.request.request_id()), result);
            work.response.clear();
            return false;
        };

        if (prepare(status, payload))
            return;

        // This is a fallback before any send, never a replay or a send retry.
        // An empty INTERNAL reply is already the smallest fallback to attempt.
        if ((status != RPC_STATUS_INTERNAL || !payload.empty()) && prepare(RPC_STATUS_INTERNAL, {}))
            return;

        log.Error(Reason::DroppedReply, "reply_dropped reason=response_preparation_failed request_id=%llu",
            static_cast<unsigned long long>(work.request.request_id()));
        // Send retires request-ID accounting even when there are no reply bytes.
    }

    void GatewayRuntime::Execute(const std::shared_ptr<Work> &work) noexcept
    {
        auto &connection = *work->connection;
        LogScope scope(
            { connection.peer.lifetime, work->request.request_id(), connection.peer.uid, work->request.service() });
        bool initialized = false, cleaned = false, expired = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            expired = work->kind == Kind::Request && Clock::now() >= work->deadline;
            if (expired)
                LogWorkerWait(*work, true);
            --reserved;
            ++running;
            if (work->kind != Kind::Request)
                ++lifecycleRunning;
            StateChanged();
        }
        try
        {
            if (work->kind == Kind::Authenticate)
            {
                if (!connection.closing)
                    initialized = adapter.Notify(connection, true);
            }
            else if (work->kind == Kind::Cleanup)
            {
                if (connection.initialized)
                    adapter.Notify(connection, false);
                cleaned = buffers.Cleanup(connection.userId);
            }
            else
            {
                // Start/expiry is decided once by Dispatch, before entering any backend.
                if (expired)
                {
                    Respond(*work, RPC_STATUS_INTERNAL);
                }
                else
                {
                    const auto result = adapter.Execute(work->connection, work->request.payload());
                    Respond(*work, result.status, result.payload);
                }
            }
        }
        catch (...)
        {
            log.Error(Reason::Allocation, "worker_failed kind=%u", static_cast<unsigned>(work->kind));
            if (work->kind == Kind::Request)
                Respond(*work, RPC_STATUS_INTERNAL);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            --running;
            if (work->kind != Kind::Request)
                --lifecycleRunning;
            if (work->kind == Kind::Authenticate)
            {
                connection.initialized = initialized;
                connection.authFinished = true;
            }
            else if (work->kind == Kind::Cleanup)
            {
                connection.cleanupFinished = cleaned;
                // A failed unlink keeps its record and quota. No backend notification replay.
                if (!cleaned)
                    log.Error(Reason::Buffer, "cleanup_incomplete uid=%u", connection.peer.uid);
            }
            else
            {
                --connection.active;
                if (completedTail)
                    completedTail->next = work;
                else
                    completedHead = work;
                completedTail = work;
            }
            StateChanged();
        }
        wake.Signal();
    }

    void GatewayRuntime::Lifecycle()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = connections.begin(); it != connections.end();)
        {
            auto connection = it->second;
            if (stopping)
                Close(connection, false);
            if (connection->closing && !connection->active && connection->authFinished)
            {
                if (connection->cleanupFinished)
                {
                    it = connections.erase(it);
                    StateChanged();
                    continue;
                }
                if (!connection->cleanupScheduled && buffers.Idle(connection->userId) && reserved + running < workers)
                {
                    auto work = std::make_shared<Work>(Kind::Cleanup, connection);
                    if (Schedule(work))
                        connection->cleanupScheduled = true;
                }
            }
            ++it;
        }
    }

    void GatewayRuntime::Io() noexcept
    {
        const auto serve = [&]
        {
            Socket router(context->Get(), ZMQ_ROUTER);
            router.ConfigureTransport();
            router.Set(ZMQ_PLAIN_SERVER, 1);
            router.Set(ZMQ_ROUTER_MANDATORY, 1);
            router.Set(ZMQ_ROUTER_HANDOVER, 0);
            if (router.Error())
            {
                Fatal("ROUTER socket/options failed");
                return;
            }
            auto versioned = RequireTransportPatch(router.Get());
            if (!versioned)
            {
                Fatal("libzmq connection-lifetime patch unavailable");
                return;
            }
            if (versioned(router.Get(), "inproc://app-eyil-gateway-monitor",
                    ZMQ_EVENT_ACCEPTED | ZMQ_EVENT_DISCONNECTED, 2, ZMQ_PAIR))
            {
                Fatal("ROUTER monitor setup failed");
                return;
            }
            struct StopMonitor
            {
                void *socket;
                MonitorFunction function;

                ~StopMonitor() { function(socket, nullptr, 0, 2, ZMQ_PAIR); }
            } stopMonitor { router.Get(), versioned };
            {
                std::unique_lock<std::mutex> lock(mutex);
                monitorEnabled = true;
                StateChanged();
                changed.wait(lock, [&] { return (monitorReady && zapReady) || stopping; });
                if (stopping)
                    return;
            }
            if (!router.Bind(endpoint.c_str()))
            {
                Fatal("ROUTER bind failed");
                return;
            }
            if (endpoint.rfind("ipc://", 0) == 0 && chmod(endpoint.substr(6).c_str(), 0666))
            {
                Fatal("ROUTER endpoint permissions failed");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                listenerReady = true;
                StateChanged();
            }
            for (;;)
            {
                bool receive;
                long timeout;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    receive = !stopping && !current;
                    timeout = completedHead ? 0 : (current ? PollTimeout(current->deadline) : -1);
                }
                // Keep the listener and existing routes until accepted work and
                // its completions drain. ZAP rejects new admission while stopping.
                zmq_pollitem_t items[] = { { router.Get(), 0, static_cast<short>(receive ? ZMQ_POLLIN : 0), 0 },
                    { nullptr, wake.Get(), ZMQ_POLLIN, 0 } };
                if (zmq_poll(items, 2, timeout) < 0 && errno != EINTR)
                {
                    Fatal("ROUTER poll failed");
                    return;
                }
                if (items[1].revents & ZMQ_POLLIN)
                    wake.Drain();
                if ((items[0].revents & ZMQ_POLLIN) && !Receive(router.Get()))
                {
                    Fatal("ROUTER receive/send failed");
                    return;
                }
                std::shared_ptr<Work> expired;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (current)
                    {
                        if (Clock::now() >= current->deadline)
                        {
                            expired = std::move(current);
                            --expired->connection->active;
                            LogWorkerWait(*expired, true);
                        }
                        else
                        {
                            try
                            {
                                if (Schedule(current))
                                    current.reset();
                                else if (!current->waitingLogged)
                                {
                                    current->waitingLogged = true;
                                    LogWorkerWait(*current, false);
                                }
                            }
                            catch (const std::bad_alloc &)
                            {
                                expired = std::move(current);
                                --expired->connection->active;
                                log.Error(Reason::Allocation, "request job allocation failed request_id=%llu",
                                    static_cast<unsigned long long>(expired->request.request_id()));
                            }
                        }
                    }
                }
                if (expired)
                {
                    Respond(*expired, RPC_STATUS_INTERNAL);
                    if (!Send(router.Get(), *expired))
                    {
                        Fatal("ROUTER reply failed");
                        return;
                    }
                }
                for (unsigned batch = 0; batch < 32; ++batch)
                {
                    std::shared_ptr<Work> work;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        work = completedHead;
                        if (work)
                        {
                            completedHead = std::move(work->next);
                            if (!completedHead)
                                completedTail.reset();
                        }
                    }
                    if (!work)
                        break;
                    if (!Send(router.Get(), *work))
                    {
                        Fatal("ROUTER reply failed");
                        return;
                    }
                }
                Lifecycle();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping && !current && !reserved && !running && !completedHead && connections.empty())
                        break;
                }
            }
        };
        try
        {
            serve();
        }
        catch (const std::exception &error)
        {
            Fatal(error.what());
        }
        catch (...)
        {
            Fatal("I/O exception");
        }
        // On transport failure there is no route to deliver to, but backend access
        // still has to finish. Do not unload workers or destroy leased allocations.
        for (;;)
        {
            uint64_t observed;
            {
                std::lock_guard<std::mutex> lock(mutex);
                observed = stateRevision;
            }
            try
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (current)
                    {
                        --current->connection->active;
                        current.reset();
                    }
                    // Destruction is iterative: an unlimited completion chain
                    // must not recursively consume the shutdown thread's stack.
                    while (completedHead)
                    {
                        auto retired = std::move(completedHead);
                        completedHead = std::move(retired->next);
                    }
                    completedTail.reset();
                    if (!reserved && !running && connections.empty())
                        break;
                }
                Lifecycle();
            }
            catch (...)
            {
                log.Error(Reason::Allocation, "shutdown cleanup deferred");
            }
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(
                lock, [&] { return stateRevision != observed || (!reserved && !running && connections.empty()); });
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            auxiliaryStop = true;
            zapWake.Signal();
            monitorWake.Signal();
            StateChanged();
        }
    }

    bool GatewayRuntime::Receive(void *router)
    {
        Message route;
        if (!route.Receive(router))
            return route.Error() == 0;
        if (!route.More())
        {
            log.Error(Reason::Protocol, "invalid ROUTER framing");
            return true;
        }
        Message body;
        if (!body.Receive(router))
        {
            log.Error(Reason::Transport, "incomplete ROUTER request errno=%d", body.Error());
            return false;
        }
        if (body.More())
        {
            bool more = true;
            while (more)
            {
                Message extra;
                if (!extra.Receive(router))
                    return false;
                more = extra.More();
            }
            log.Error(Reason::Protocol, "multipart request rejected");
            return true;
        }
        std::shared_ptr<Work> work;
        bool correlated = false;
        RpcStatus status = RPC_STATUS_INTERNAL;
        try
        {
            uint64_t id = 0, fd = 0, routeId = 0, routeFd = 0;
            const char *user = body.Property("User-Id");
            if (!user || !*user || !Decimal(body.Property("Transport-Connection-Id"), id) || !id
                || !Decimal(body.Property("Transport-FD"), fd) || fd > INT_MAX
                || !Decimal(route.Property("Transport-Connection-Id"), routeId) || routeId != id
                || !Decimal(route.Property("Transport-FD"), routeFd) || routeFd != fd)
            {
                log.Error(Reason::Protocol, "missing authenticated binding");
                return true;
            }
            std::shared_ptr<Connection> connection;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto it = connections.find(id);
                if (it == connections.end() || it->second->closing || !it->second->authenticated
                    || it->second->peer.fd != static_cast<int>(fd) || it->second->userId != user)
                {
                    log.Error(Reason::Protocol, "stale authenticated binding");
                    return true;
                }
                connection = it->second;
            }
            work = std::make_shared<Work>(Kind::Request, connection);
            work->route = route.String();
            work->admitted = Clock::now();
            work->deadline = work->admitted + WorkerTimeout;
            if (body.Size() > INT_MAX || !work->request.ParseFromArray(body.Data(), static_cast<int>(body.Size()))
                || !work->request.request_id())
            {
                log.Error(Reason::Protocol, "uncorrelatable request");
                return true;
            }
            // Check ownership before schema/service rejection as well: any reply
            // with an occupied ID could consume the original client's waiter.
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (connection->requests.count(work->request.request_id()))
                {
                    log.Error(Reason::Protocol, "duplicate_request_id request_id=%llu connection=%llu",
                        static_cast<unsigned long long>(work->request.request_id()),
                        static_cast<unsigned long long>(connection->peer.lifetime));
                    return true;
                }
            }
            correlated = true;
            status = RPC_STATUS_OK;
            if (work->request.schema_version() != EYIL_RPC_SCHEMA_VERSION)
                status = RPC_STATUS_UNSUPPORTED_SCHEMA;
            else if (work->request.service() != RPC_SERVICE_CRYPTO)
                status = RPC_STATUS_UNSUPPORTED_SERVICE;
            if (status == RPC_STATUS_OK)
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping || connection->closing)
                    status = RPC_STATUS_INVALID_CONNECTION;
                else
                {
                    // Only the ROUTER owner inserts/retires request IDs.
                    connection->requests.insert(work->request.request_id());
                    work->tracked = true;
                    ++connection->active;
                    current = work;
                    return true;
                }
                log.Error(Reason::Protocol, "request rejected status=%d id=%llu", status,
                    static_cast<unsigned long long>(work->request.request_id()));
            }
        }
        catch (const std::bad_alloc &)
        {
            log.Error(Reason::Allocation, "request_admission_allocation_failed correlated=%u", correlated ? 1u : 0u);
            status = RPC_STATUS_INTERNAL;
        }
        catch (const std::exception &e)
        {
            log.Error(Reason::Protocol, "request admission failed: %s", e.what());
            status = RPC_STATUS_INTERNAL;
        }
        if (correlated)
        {
            Respond(*work, status);
            // A send/route-guard failure must not be caught as an admission error
            // and followed by a second send on a potentially partial multipart.
            return Send(router, *work);
        }
        return true;
    }

    bool GatewayRuntime::Send(void *router, const Work &work)
    {
        const auto &connection = work.connection;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (work.tracked)
                connection->requests.erase(work.request.request_id());
            if (connection->disconnected || work.response.empty())
                return true;
        }
        const auto lifetime = connection->peer.lifetime;
        if (zmq_setsockopt(router, ExpectedConnection, &lifetime, sizeof(lifetime)))
        {
            log.Error(Reason::Transport, "route guard failed errno=%d", errno);
            return false;
        }
        if (zmq_send(router, work.route.data(), work.route.size(), ZMQ_SNDMORE | ZMQ_DONTWAIT) < 0)
        {
            log.Error(Reason::DroppedReply, "reply_dropped errno=%d request_id=%llu", errno,
                static_cast<unsigned long long>(work.request.request_id()));
            return true;
        }
        if (zmq_send(router, work.response.data(), work.response.size(), ZMQ_DONTWAIT) < 0)
        {
            log.Error(Reason::DroppedReply, "multipart_reply_failed errno=%d", errno);
            return false; // The owner must close a partially sent multipart socket.
        }
        return true;
    }

    void GatewayRuntime::Monitor() noexcept
    {
        try
        {
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return monitorEnabled || stopping; });
                if (stopping)
                    return;
            }
            Socket pair(context->Get(), ZMQ_PAIR);
            if (!pair.Connect("inproc://app-eyil-gateway-monitor"))
            {
                Fatal("monitor socket setup failed");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                monitorReady = true;
                StateChanged();
            }
            for (;;)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (auxiliaryStop)
                        break;
                }
                zmq_pollitem_t items[]
                    = { { pair.Get(), 0, ZMQ_POLLIN, 0 }, { nullptr, monitorWake.Get(), ZMQ_POLLIN, 0 } };
                if (zmq_poll(items, 2, -1) < 0 && errno != EINTR)
                {
                    Fatal("monitor poll failed");
                    return;
                }
                if (items[1].revents & ZMQ_POLLIN)
                    monitorWake.Drain();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (auxiliaryStop)
                        break;
                }
                if (!(items[0].revents & ZMQ_POLLIN))
                    continue;
                MonitorEvent event;
                const auto read = ReadMonitor(pair.Get(), event);
                if (read == MonitorRead::Empty)
                    continue;
                if (read == MonitorRead::Error)
                {
                    Fatal("invalid monitor event");
                    return;
                }
                std::unique_lock<std::mutex> lock(mutex);
                if (event.event == ZMQ_EVENT_ACCEPTED)
                {
                    if (event.values.size() != 2 || !event.values[1] || event.values[0] > INT_MAX)
                    {
                        lock.unlock();
                        Fatal("invalid ACCEPTED binding");
                        return;
                    }
                    auto connection = std::make_shared<Connection>();
                    connection->peer.fd = event.values[0];
                    connection->peer.lifetime = event.values[1];
                    connection->authFinished = true; // no ZAP work yet
                    connections.emplace(connection->peer.lifetime, connection);
                    if (stopping)
                        Close(connection);
                }
                else if (event.event == ZMQ_EVENT_DISCONNECTED)
                {
                    if (event.values.size() != 5)
                    {
                        lock.unlock();
                        Fatal("invalid DISCONNECTED binding");
                        return;
                    }
                    auto it = connections.find(event.values[4]);
                    if (it != connections.end())
                    {
                        if (it->second->peer.fd != static_cast<int>(event.values[0]))
                        {
                            lock.unlock();
                            Fatal("disconnect FD mismatch");
                            return;
                        }
                        Close(it->second);
                    }
                }
                StateChanged();
                wake.Signal();
            }
        }
        catch (const std::exception &error)
        {
            Fatal(error.what());
        }
        catch (...)
        {
            Fatal("monitor exception");
        }
    }

    void GatewayRuntime::Authenticate() noexcept
    {
        try
        {
            Socket rep(context->Get(), ZMQ_REP);
            if (!rep.Bind("inproc://zeromq.zap.01"))
            {
                Fatal("ZAP socket setup failed");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                zapReady = true;
                StateChanged();
            }
            for (;;)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (auxiliaryStop)
                        break;
                }
                zmq_pollitem_t items[] = { { rep.Get(), 0, ZMQ_POLLIN, 0 }, { nullptr, zapWake.Get(), ZMQ_POLLIN, 0 } };
                if (zmq_poll(items, 2, -1) < 0 && errno != EINTR)
                {
                    Fatal("ZAP poll/send failed");
                    return;
                }
                if (items[1].revents & ZMQ_POLLIN)
                    zapWake.Drain();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (auxiliaryStop)
                        break;
                }
                if (!(items[0].revents & ZMQ_POLLIN))
                    continue;
                std::vector<std::string> frames;
                Peer peer;
                bool credentialsValid = false;
                std::string status = "400", user;
                std::shared_ptr<Connection> connection;
                try
                {
                    for (;;)
                    {
                        Message frame;
                        if (!frame.Receive(rep.Get()))
                        {
                            Fatal("incomplete ZAP request");
                            return;
                        }
                        if (frames.size() == 3)
                            credentialsValid = Credentials(frame, frame.String(), peer);
                        if (frames.size() < 9)
                            frames.push_back(frame.String());
                        if (!frame.More())
                            break;
                    }
                    if (!credentialsValid || frames.size() != 8 || frames[0] != "1.0" || frames[5] != "PLAIN")
                    {
                        status = "400";
                    }
                    else
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        // Monitor ACCEPTED is the authoritative live binding. If an early
                        // disconnect already removed it, late ZAP cannot recreate it.
                        changed.wait_for(
                            lock, CallTimeout, [&] { return connections.count(peer.lifetime) || stopping; });
                        const auto it = connections.find(peer.lifetime);
                        if (stopping || it == connections.end() || it->second->closing
                            || it->second->peer.fd != peer.fd)
                            status = "300";
                        else
                        {
                            connection = it->second;
                            connection->peer = peer;
                            connection->userId = Token();
                            const auto appToken = Token();
                            connection->appId = "eyil-interim:" + appToken;
                            connection->sessionId = Token();
                            if (connection->userId.empty() || appToken.empty() || connection->sessionId.empty())
                            {
                                status = "500";
                                Close(connection);
                            }
                            else
                            {
                                connection->authFinished = false;
                                auto work = std::make_shared<Work>(Kind::Authenticate, connection);
                                changed.wait(lock,
                                    [&] { return stopping || connection->closing || reserved + running < workers; });
                                if (!stopping && !connection->closing && Schedule(work))
                                {
                                    changed.wait(lock,
                                        [&] { return stopping || connection->closing || connection->authFinished; });
                                    if (!stopping && !connection->closing && connection->authFinished)
                                    {
                                        // Backend availability is not transport authentication.
                                        // A missing Crypto interface is reported by individual RPCs;
                                        // local buffer operations remain available on this connection.
                                        status = "200";
                                        user = connection->userId;
                                        connection->authenticated = true;
                                    }
                                    else
                                        status = "500";
                                }
                                else
                                {
                                    connection->authFinished = true;
                                    status = "300";
                                }
                            }
                            if (status != "200")
                                Close(connection);
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    log.Error(Reason::Authentication, "authentication_failed: %s", e.what());
                    status = "500";
                    if (connection)
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        connection->authFinished = true;
                        Close(connection);
                    }
                }
                if (status != "200")
                    log.Error(Reason::Authentication, "ZAP rejected status=%s", status.c_str());
                const std::string response[] = { "1.0", frames.size() > 1 ? frames[1] : "", status,
                    status == "200" ? "OK" : "Rejected", user, "" };
                for (unsigned i = 0; i < 6; ++i)
                    if (zmq_send(
                            rep.Get(), response[i].data(), response[i].size(), ZMQ_DONTWAIT | (i < 5 ? ZMQ_SNDMORE : 0))
                        < 0)
                    {
                        Fatal("ZAP poll/send failed");
                        return;
                    }
            }
        }
        catch (const std::exception &error)
        {
            Fatal(error.what());
        }
        catch (...)
        {
            Fatal("ZAP exception");
        }
    }
}
