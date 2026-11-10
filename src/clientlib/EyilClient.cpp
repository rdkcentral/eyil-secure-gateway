/**
 * If not stated otherwise in this file or this component's LICENSE
 * file the following copyright and licenses apply:
 *
 * Copyright 2026 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <eyilclient/EyilClient.h>
#include "../common/RuntimeSupport.h"
#include <ProtocolVersion.h>
#include <rpc.pb.h>

#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <thread>
#include <unordered_map>

namespace
{
    Eyil::Diagnostics diagnostics("EyilClient");

    struct Request
    {
        RpcRequest envelope;
        std::string wire;
        Eyil::Clock::time_point deadline;

        bool asynchronous = false;
        bool text = false;
        bool accepted = false;
        bool done = false;
        uint64_t id = 0;
        ClientResult result = CLIENT_COMMUNICATION_ERROR;

        ClientResponseHandler binaryHandler = nullptr;
        ClientStringResponseHandler stringHandler = nullptr;

        void *response = nullptr;
        size_t responseSize = 0;

        ~Request() { std::free(response); }
    };

    ClientResult SystemResult(int error) noexcept
    {
        return error == ENOMEM ? CLIENT_OUT_OF_MEMORY : CLIENT_COMMUNICATION_ERROR;
    }

    ClientResult ExceptionResult() noexcept
    {
        try
        {
            std::rethrow_exception(std::current_exception());
        }
        catch (const std::bad_alloc &)
        {
            return CLIENT_OUT_OF_MEMORY;
        }
        catch (const std::system_error &e)
        {
            return SystemResult(e.code().value());
        }
        catch (...)
        {
            return CLIENT_COMMUNICATION_ERROR;
        }
    }
}

struct Client
{
    Eyil::Wakeup wake;
    std::mutex mutex;
    std::mutex stopMutex;
    std::condition_variable changed;
    std::thread owner;
    std::thread::id ownerId;

    ClientConnectionEventHandler connectionHandler = nullptr;
    Eyil::Clock::time_point startupDeadline;

    bool startupPending = true;
    ClientResult startupResult = CLIENT_COMMUNICATION_ERROR;
    bool published = false;
    bool ready = false;
    bool stop = false;
    bool notifyLost = false;

    size_t activeCalls = 0;
    uint64_t nextId = 1;
    std::shared_ptr<Request> slot;

    // Only the owner mutates this map. Request transitions also hold mutex so
    // a caller's deadline cannot cancel a successful nonblocking send.
    std::unordered_map<uint64_t, std::shared_ptr<Request>> pending;

    ~Client() { Stop(); }

    void Complete(Request &request, ClientResult result)
    {
        if (!request.done)
        {
            request.result = result;
            request.done = true;
            changed.notify_all();
        }
    }

    void Startup(ClientResult result)
    {
        if (startupPending)
        {
            startupPending = false;
            startupResult = result;
            changed.notify_all();
        }
    }

    void Stop() noexcept
    {
        try
        {
            std::lock_guard<std::mutex> serial(stopMutex);
            {
                std::lock_guard<std::mutex> lock(mutex);
                stop = true;
                ready = false;
                changed.notify_all();
            }
            wake.Signal();
            if (owner.joinable())
                owner.join();
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return activeCalls == 0; });
        }
        catch (...)
        {
            std::terminate();
        }
    }

    void Fail(ClientResult result)
    {
        std::lock_guard<std::mutex> lock(mutex);
        notifyLost = notifyLost || ready;
        ready = false;
        Startup(result);
        if (slot)
            Complete(*slot, result);
        slot.reset();
        for (auto &item : pending)
            Complete(*item.second, result);
        pending.clear();
        changed.notify_all();
    }

    void Send(void *dealer)
    {
        std::shared_ptr<Request> request;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!ready || stop || !slot)
                return;
            request = slot;
        }
        const auto reject = [&](ClientResult result)
        {
            std::lock_guard<std::mutex> lock(mutex);
            Complete(*request, result);
            pending.erase(request->id);
            slot.reset();
            changed.notify_all();
        };
        // Serialization and map growth do not hold the callers' state lock.
        // Only the short nonblocking send/acceptance transition excludes cancellation.
        try
        {
            if (!request->id)
            {
                if (!nextId)
                {
                    reject(CLIENT_COMMUNICATION_ERROR);
                    return;
                }
                request->id = nextId++;
                request->envelope.set_request_id(request->id);
                request->envelope.set_schema_version(EYIL_RPC_SCHEMA_VERSION);
                if (!request->envelope.SerializeToString(&request->wire))
                {
                    reject(CLIENT_OUT_OF_MEMORY);
                    return;
                }
                pending.emplace(request->id, request);
            }
            std::lock_guard<std::mutex> lock(mutex);
            auto retire = [&](ClientResult result)
            {
                Complete(*request, result);
                pending.erase(request->id);
                slot.reset();
                changed.notify_all();
            };
            if (stop || !ready)
            {
                retire(stop ? CLIENT_NOT_CONNECTED : CLIENT_COMMUNICATION_ERROR);
                return;
            }
            if (request->done || Eyil::Clock::now() >= request->deadline)
            {
                retire(CLIENT_TIMEOUT);
                return;
            }
            if (zmq_send(dealer, request->wire.data(), request->wire.size(), ZMQ_DONTWAIT) >= 0)
            {
                request->accepted = true;
                if (request->asynchronous)
                    Complete(*request, CLIENT_OK);
                slot.reset();
                changed.notify_all();
            }
            else if (errno != EAGAIN && errno != EINTR)
                retire(errno == ENOMEM ? CLIENT_OUT_OF_MEMORY : CLIENT_COMMUNICATION_ERROR);
        }
        catch (...)
        {
            const auto result = ExceptionResult();
            std::lock_guard<std::mutex> lock(mutex);
            Complete(*request, result);
            pending.erase(request->id);
            slot.reset();
            changed.notify_all();
        }
    }

    void Expire()
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto now = Eyil::Clock::now();
        for (auto it = pending.begin(); it != pending.end();)
        {
            auto &r = *it->second;
            const bool hasDeadline = !r.asynchronous || !r.accepted;
            if (hasDeadline && (r.done || now >= r.deadline))
            {
                Complete(r, CLIENT_TIMEOUT);
                if (slot == it->second)
                {
                    slot.reset();
                    changed.notify_all();
                }
                it = pending.erase(it);
            }
            else
                ++it;
        }
        if (slot && (slot->done || now >= slot->deadline))
        {
            Complete(*slot, CLIENT_TIMEOUT);
            slot.reset();
            changed.notify_all();
        }
    }

    bool Receive(void *dealer)
    {
        Eyil::Message message;
        if (!message.Receive(dealer))
        {
            if (message.Error())
                Fail(SystemResult(message.Error()));
            return false;
        }
        if (message.More() || message.Size() > INT_MAX)
        {
            diagnostics.Error(Eyil::Reason::Protocol, "invalid response framing");
            Fail(CLIENT_COMMUNICATION_ERROR);
            return false;
        }
        RpcResponse envelope;
        if (!envelope.ParseFromArray(message.Data(), static_cast<int>(message.Size())) || !envelope.request_id())
        {
            diagnostics.Error(Eyil::Reason::Protocol, "uncorrelatable response");
            Fail(CLIENT_COMMUNICATION_ERROR);
            return false;
        }
        std::shared_ptr<Request> request;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = pending.find(envelope.request_id());
            if (it == pending.end())
                return true;
            request = it->second;
            pending.erase(it);
            if (!request->accepted)
            {
                Complete(*request, CLIENT_COMMUNICATION_ERROR);
                if (slot == request)
                {
                    slot.reset();
                    changed.notify_all();
                }
                diagnostics.Error(Eyil::Reason::Protocol, "response_before_send_acceptance");
                return true;
            }
            if (stop || (!request->asynchronous && request->done))
                return true;
        }
        void *owned = nullptr;
        size_t ownedSize = 0;
        ClientResult result = CLIENT_OK;
        try
        {
            if (envelope.service() != request->envelope.service()
                || envelope.schema_version() != EYIL_RPC_SCHEMA_VERSION || !RpcStatus_IsValid(envelope.status())
                || envelope.status() == RPC_STATUS_UNSPECIFIED)
                result = CLIENT_COMMUNICATION_ERROR;
            else if (request->text && (envelope.status() == RPC_STATUS_OK || !envelope.payload().empty()))
                result = CLIENT_COMMUNICATION_ERROR;
            else
            {
                std::string status;
                if (request->text)
                    status = RpcStatus_Name(envelope.status());
                ownedSize = request->text ? status.size() + 1 : message.Size();
                owned = std::malloc(ownedSize);
                if (!owned)
                    result = CLIENT_OUT_OF_MEMORY;
                else
                    std::memcpy(
                        owned, request->text ? static_cast<const void *>(status.c_str()) : message.Data(), ownedSize);
            }
        }
        catch (...)
        {
            result = ExceptionResult();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stop || (!request->asynchronous && request->done))
            {
                std::free(owned);
                return true;
            }
            if (!request->asynchronous && Eyil::Clock::now() >= request->deadline)
                result = CLIENT_TIMEOUT;
            if (result != CLIENT_OK)
            {
                std::free(owned);
                diagnostics.Error(Eyil::Reason::Protocol, "response_retired id=%llu local_result=%u",
                    static_cast<unsigned long long>(request->id), result);
                if (!request->asynchronous)
                    Complete(*request, result);
                return true;
            }
            request->responseSize = ownedSize;
            if (!request->asynchronous)
            {
                request->response = owned;
                Complete(*request, CLIENT_OK);
                return true;
            }
        }
        // Response ownership passes to application code. Never invoke it under
        // the coordinator lock or on the submitting thread.
        try
        {
            if (request->text)
                request->stringHandler(this, static_cast<char *>(owned));
            else
                request->binaryHandler(this, static_cast<uint8_t *>(owned), request->responseSize);
        }
        catch (...)
        {
            diagnostics.Error(Eyil::Reason::Callback, "response_callback_threw");
        }
        return true;
    }

    bool Monitor(void *monitor)
    {
        Eyil::Message event;
        if (!event.Receive(monitor))
        {
            if (event.Error())
                Fail(SystemResult(event.Error()));
            return false;
        }
        if (event.Size() != 6 || !event.More())
        {
            Fail(CLIENT_COMMUNICATION_ERROR);
            return false;
        }
        uint16_t kind;
        int32_t value;
        std::memcpy(&kind, event.Data(), sizeof(kind));
        std::memcpy(&value, static_cast<const char *>(event.Data()) + 2, sizeof(value));
        Eyil::Message endpoint;
        while (!endpoint.Receive(monitor))
        {
            if (endpoint.Error() || errno != EINTR)
            {
                Fail(SystemResult(endpoint.Error()));
                return false;
            }
        }
        if (endpoint.More())
        {
            Fail(CLIENT_COMMUNICATION_ERROR);
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (stop)
            return true;
        switch (kind)
        {
        case ZMQ_EVENT_HANDSHAKE_SUCCEEDED:
            if (startupPending)
            {
                ready = Eyil::Clock::now() < startupDeadline;
                Startup(ready ? CLIENT_OK : CLIENT_TIMEOUT);
            }
            break;
        case ZMQ_EVENT_HANDSHAKE_FAILED_AUTH:
            Startup(value == 400 ? CLIENT_AUTH_FAILED : CLIENT_COMMUNICATION_ERROR);
            ready = false;
            break;
        case ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL:
        case ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
        case ZMQ_EVENT_DISCONNECTED:
            notifyLost = notifyLost || ready;
            ready = false;
            Startup(CLIENT_COMMUNICATION_ERROR);
            break;
        default:
            break;
        }
        changed.notify_all();
        return true;
    }

    void Run() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ownerId = std::this_thread::get_id();
        }
        const auto serve = [&]
        {
            if (wake.Error())
            {
                Fail(SystemResult(wake.Error()));
                return;
            }
            Eyil::Context context;
            if (context.Error())
            {
                Fail(SystemResult(context.Error()));
                return;
            }
            Eyil::Socket dealer(context.Get(), ZMQ_DEALER);
            dealer.ConfigureTransport();
            dealer.Set(ZMQ_RECONNECT_IVL, -1);
            dealer.Set(ZMQ_IMMEDIATE, 1);
            dealer.SetString(ZMQ_PLAIN_USERNAME, "eyil");
            dealer.SetString(ZMQ_PLAIN_PASSWORD, "transport");
            if (dealer.Error())
            {
                Fail(SystemResult(dealer.Error()));
                return;
            }
            const auto token = Eyil::Token();
            if (token.empty())
            {
                Fail(SystemResult(errno));
                return;
            }
            const std::string address = "inproc://app-eyil-client-monitor-" + token;
            Eyil::Socket monitor(context.Get(), ZMQ_PAIR);
            if (monitor.Error())
            {
                Fail(SystemResult(monitor.Error()));
                return;
            }
            if (zmq_socket_monitor(dealer.Get(), address.c_str(), ZMQ_EVENT_ALL))
            {
                Fail(SystemResult(errno));
                return;
            }
            struct StopMonitor
            {
                void *dealer;
                ~StopMonitor() { zmq_socket_monitor(dealer, nullptr, 0); }
            } stopMonitor { dealer.Get() };
            monitor.Connect(address.c_str());
            dealer.Connect(Eyil::Endpoint);
            if (monitor.Error() || dealer.Error())
            {
                Fail(SystemResult(monitor.Error() ? monitor.Error() : dealer.Error()));
                return;
            }
            for (;;)
            {
                bool sending;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stop || (!startupPending && !ready))
                        break;
                    sending = ready && static_cast<bool>(slot);
                }
                zmq_pollitem_t items[]
                    = { { dealer.Get(), 0, static_cast<short>(ZMQ_POLLIN | (sending ? ZMQ_POLLOUT : 0)), 0 },
                          { monitor.Get(), 0, ZMQ_POLLIN, 0 }, { nullptr, wake.Get(), ZMQ_POLLIN, 0 } };
                const int rc = zmq_poll(items, 3, 50);
                if (rc < 0 && errno != EINTR)
                {
                    Fail(SystemResult(errno));
                    return;
                }
                if (items[2].revents)
                    wake.Drain();
                for (int i = 0; i < 16 && Monitor(monitor.Get()); ++i)
                {
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stop || (!startupPending && !ready))
                        break;
                }
                Expire();
                Send(dealer.Get());
                for (int i = 0; i < 16 && Receive(dealer.Get()); ++i)
                {
                }
            }
        };
        try
        {
            serve();
        }
        catch (...)
        {
            const auto error = ExceptionResult();
            diagnostics.Error(Eyil::Reason::Transport, "io_stopped local_result=%u", error);
            Fail(error);
        }
        ClientConnectionEventHandler callback = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex);
            const auto error = stop ? CLIENT_NOT_CONNECTED : CLIENT_COMMUNICATION_ERROR;
            ready = false;
            Startup(error);
            if (slot)
                Complete(*slot, error);
            slot.reset();
            for (auto &item : pending)
                Complete(*item.second, error);
            pending.clear();
            changed.notify_all();
            if (notifyLost && !stop)
            {
                changed.wait(lock, [&] { return published || stop; });
                if (!stop)
                    callback = connectionHandler;
            }
        }
        if (callback)
        {
            try
            {
                callback(this, CLIENT_CONNECTION_LOST);
            }
            catch (...)
            {
                diagnostics.Error(Eyil::Reason::Callback, "connection_callback_threw");
            }
        }
    }
};

namespace
{
    std::mutex handlesMutex;
    std::unordered_map<Client *, std::shared_ptr<Client>> handles;

    struct Call
    {
        std::shared_ptr<Client> client;

        explicit Call(Client *handle)
        {
            std::lock_guard<std::mutex> registry(handlesMutex);
            auto it = handles.find(handle);
            if (it == handles.end())
                return;
            client = it->second;
            std::lock_guard<std::mutex> lock(client->mutex);
            ++client->activeCalls;
        }

        ~Call()
        {
            if (client)
            {
                std::lock_guard<std::mutex> lock(client->mutex);
                --client->activeCalls;
                client->changed.notify_all();
            }
        }
    };

    ClientResult Submit(Client *handle, const void *input, size_t size, bool text, bool asynchronous,
        ClientResponseHandler binary, ClientStringResponseHandler string, void **output, size_t *outputSize)
    {
        const auto deadline = Eyil::Clock::now() + Eyil::CallTimeout;
        if (!handle || !input || (!text && !size) || (asynchronous && !binary && !string))
            return CLIENT_INVALID_ARGUMENT;
        Call call(handle);
        if (!call.client)
            return CLIENT_INVALID_ARGUMENT;
        Client &client = *call.client;
        {
            std::lock_guard<std::mutex> lock(client.mutex);
            if (client.ownerId == std::this_thread::get_id())
                return CLIENT_INVALID_ARGUMENT;
        }
        auto request = std::make_shared<Request>();
        request->deadline = deadline;
        request->asynchronous = asynchronous;
        request->text = text;
        request->binaryHandler = binary;
        request->stringHandler = string;
        if (text)
        {
            size = std::strlen(static_cast<const char *>(input));
            if (size > INT_MAX)
                return CLIENT_INVALID_ARGUMENT;
            request->envelope.set_service(RPC_SERVICE_UNSPECIFIED);
            request->envelope.set_payload(input, size);
        }
        else if (size > INT_MAX || !request->envelope.ParseFromArray(input, static_cast<int>(size))
            || request->envelope.service() == RPC_SERVICE_UNSPECIFIED)
            return CLIENT_INVALID_ARGUMENT;
        std::unique_lock<std::mutex> lock(client.mutex);
        if (!client.ready || client.stop)
            return CLIENT_NOT_CONNECTED;
        while (client.slot && client.ready && !client.stop)
        {
            if (client.changed.wait_until(lock, deadline) == std::cv_status::timeout)
                return CLIENT_TIMEOUT;
        }
        if (!client.ready || client.stop)
            return client.stop ? CLIENT_NOT_CONNECTED : CLIENT_COMMUNICATION_ERROR;
        if (Eyil::Clock::now() >= deadline)
            return CLIENT_TIMEOUT;
        client.slot = request;
        client.wake.Signal();
        while (!request->done)
        {
            if (client.stop)
                client.Complete(*request, CLIENT_NOT_CONNECTED);
            else if (!client.ready)
                client.Complete(*request, CLIENT_COMMUNICATION_ERROR);
            else if (client.changed.wait_until(lock, deadline) == std::cv_status::timeout && !request->done)
                client.Complete(*request, CLIENT_TIMEOUT);
        }
        if (request->result == CLIENT_OK && !asynchronous)
        {
            *output = request->response;
            request->response = nullptr;
            if (outputSize)
                *outputSize = request->responseSize;
        }
        client.wake.Signal();
        return request->result;
    }
}

extern "C"
{
    ClientResult client_connect(Client **handle, ClientConnectionEventHandler handler)
    {
        if (!handle)
            return CLIENT_INVALID_ARGUMENT;
        if (*handle)
        {
            std::lock_guard<std::mutex> lock(handlesMutex);
            return handles.count(*handle) ? CLIENT_ALREADY_CONNECTED : CLIENT_INVALID_ARGUMENT;
        }
        std::shared_ptr<Client> client;
        try
        {
            const auto deadline = Eyil::Clock::now() + Eyil::CallTimeout;
            client = std::make_shared<Client>();
            client->connectionHandler = handler;
            client->startupDeadline = deadline;
            client->owner = std::thread([ptr = client.get()] { ptr->Run(); });
            ClientResult result;
            {
                std::unique_lock<std::mutex> lock(client->mutex);
                if (!client->changed.wait_until(lock, deadline, [&] { return !client->startupPending; }))
                    client->Startup(CLIENT_TIMEOUT);
                result = client->startupResult;
            }
            if (result != CLIENT_OK)
            {
                client->Stop();
                return result;
            }
            {
                std::lock_guard<std::mutex> lock(handlesMutex);
                handles.emplace(client.get(), client);
            }
            {
                std::lock_guard<std::mutex> lock(client->mutex);
                *handle = client.get();
                client->published = true;
                client->changed.notify_all();
            }
            return CLIENT_OK;
        }
        catch (...)
        {
            const auto error = ExceptionResult();
            if (client)
                client->Stop();
            *handle = nullptr;
            return error;
        }
    }

    ClientResult client_send_request(
        Client *client, const uint8_t *request, size_t size, uint8_t **response, size_t *responseSize)
    {
        if (response)
            *response = nullptr;
        if (responseSize)
            *responseSize = 0;
        if (!response || !responseSize)
            return CLIENT_INVALID_ARGUMENT;
        try
        {
            void *owned = nullptr;
            const auto result = Submit(client, request, size, false, false, nullptr, nullptr, &owned, responseSize);
            *response = static_cast<uint8_t *>(owned);
            return result;
        }
        catch (...)
        {
            return ExceptionResult();
        }
    }

    ClientResult client_send_request_async(
        Client *client, const uint8_t *request, size_t size, ClientResponseHandler handler)
    {
        try
        {
            return Submit(client, request, size, false, true, handler, nullptr, nullptr, nullptr);
        }
        catch (...)
        {
            return ExceptionResult();
        }
    }

    ClientResult client_send_request_string(Client *client, const char *request, char **response)
    {
        if (!response)
            return CLIENT_INVALID_ARGUMENT;
        *response = nullptr;
        try
        {
            void *owned = nullptr;
            const auto result = Submit(client, request, 0, true, false, nullptr, nullptr, &owned, nullptr);
            *response = static_cast<char *>(owned);
            return result;
        }
        catch (...)
        {
            return ExceptionResult();
        }
    }

    ClientResult client_send_request_string_async(
        Client *client, const char *request, ClientStringResponseHandler handler)
    {
        try
        {
            return Submit(client, request, 0, true, true, nullptr, handler, nullptr, nullptr);
        }
        catch (...)
        {
            return ExceptionResult();
        }
    }

    void client_free_response(void *response) { std::free(response); }

    ClientResult client_register_notifications(Client *, ClientNotificationHandler) { return CLIENT_NOT_IMPLEMENTED; }

    void client_unregister_notifications(Client *) { }

    void client_disconnect(Client *handle)
    {
        std::shared_ptr<Client> client;
        {
            std::lock_guard<std::mutex> registry(handlesMutex);
            const auto it = handles.find(handle);
            if (it == handles.end())
                return;
            client = it->second;
            {
                std::lock_guard<std::mutex> lock(client->mutex);
                if (client->ownerId == std::this_thread::get_id())
                {
                    diagnostics.Error(Eyil::Reason::Callback, "disconnect_from_callback_rejected");
                    return;
                }
            }
            handles.erase(it);
        }
        client->Stop();
    }
}
