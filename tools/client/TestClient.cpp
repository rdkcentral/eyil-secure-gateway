// Copyright 2026 RDK Management
// SPDX-License-Identifier: Apache-2.0
#include "TestClient.h"

#include <climits>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace EyilDev
{
    namespace
    {
        struct FreeResponse
        {
            void operator()(void *data) const { client_free_response(data); }
        };

        using ResponseStorage = std::unique_ptr<uint8_t, FreeResponse>;
        constexpr auto ResponseWait = std::chrono::seconds(30);
    }

    Runner *Runner::activeRunner = nullptr;

    void Check(bool condition, const std::string &message)
    {
        if (!condition)
            throw Failure(message);
    }

    const char *Name(ClientResult result)
    {
        switch (result)
        {
        case CLIENT_OK:
            return "CLIENT_OK";
        case CLIENT_INVALID_ARGUMENT:
            return "CLIENT_INVALID_ARGUMENT";
        case CLIENT_NOT_CONNECTED:
            return "CLIENT_NOT_CONNECTED";
        case CLIENT_AUTH_FAILED:
            return "CLIENT_AUTH_FAILED";
        case CLIENT_TIMEOUT:
            return "CLIENT_TIMEOUT";
        case CLIENT_COMMUNICATION_ERROR:
            return "CLIENT_COMMUNICATION_ERROR";
        case CLIENT_OUT_OF_MEMORY:
            return "CLIENT_OUT_OF_MEMORY";
        case CLIENT_ALREADY_CONNECTED:
            return "CLIENT_ALREADY_CONNECTED";
        case CLIENT_NOT_IMPLEMENTED:
            return "CLIENT_NOT_IMPLEMENTED";
        }
        return "UNKNOWN_CLIENT_RESULT";
    }

    const char *Name(Delivery delivery) { return delivery == Delivery::Sync ? "sync" : "async"; }

    RpcRequest Envelope(const CryptoRequest &request)
    {
        RpcRequest rpc;
        rpc.set_service(RPC_SERVICE_CRYPTO);
        rpc.set_payload(request.SerializeAsString());
        return rpc;
    }

    BufferRegion Region(uint32_t offset, uint32_t length)
    {
        BufferRegion result;
        result.set_offset(offset);
        result.set_length(length);
        return result;
    }

    Connection::Connection(Runner &runner, std::string label)
        : runner(runner)
        , label(std::move(label))
    {
        Connect();
    }

    Connection::~Connection()
    {
        if (client)
        {
            client_disconnect(client);
            client = nullptr;
        }
    }

    void Connection::Connect()
    {
        try
        {
            const auto result = client_connect(&client, Runner::Event);
            runner.Log("CONNECT connection=" + label + " local=" + Name(result));
            if (result != CLIENT_OK || !client)
                throw Failure("cannot establish connection " + label + ": " + Name(result), true);
            responseIds.clear();
            runner.Health();
        }
        catch (...)
        {
            // The constructor's destructor will not run after a failed Connect,
            // including a startup callback contract failure or logging allocation.
            if (client)
            {
                client_disconnect(client);
                client = nullptr;
            }
            throw;
        }
    }

    void Connection::Close()
    {
        if (!client)
            return;
        client_disconnect(client);
        client = nullptr;
        runner.Log("DISCONNECT connection=" + label + " completed; callbacks quiesced");
        runner.Health();
    }

    Runner::Runner(Mode mode)
        : mode(mode)
    {
        activeRunner = this;
    }

    Runner::~Runner()
    {
        // Connections are destroyed first, so no callback can enter this object.
        for (auto &record : pending)
            client_free_response(record.data);
        client_free_response(stringData);
        activeRunner = nullptr;
    }

    void Runner::Log(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << message << std::endl;
    }

    void Runner::Run(const std::string &name, const std::string &expected, const std::function<void()> &test)
    {
        if (stopped)
        {
            Skip(name, "previous fatal failure");
            return;
        }
        Log("RUN case=" + name + " expected=" + expected);
        const auto start = std::chrono::steady_clock::now();
        try
        {
            Health();
            test();
            Health();
            if (stopped)
                return;
            ++passed;
            const auto elapsed
                = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                      .count();
            Log("PASS case=" + name + " duration_ms=" + std::to_string(elapsed));
        }
        catch (const Failure &error)
        {
            ++failed;
            stopped = stopped || error.fatal;
            unsafe = unsafe || error.fatal;
            Log(std::string(error.fatal ? "FATAL" : "FAIL") + " case=" + name + " error=" + error.what());
        }
        catch (const std::exception &error)
        {
            Abort("case=" + name + " exception=" + error.what());
        }
    }

    void Runner::Skip(const std::string &name, const std::string &reason)
    {
        ++skipped;
        Log("SKIP case=" + name + " reason=" + reason);
    }

    void Runner::Abort(const std::string &message)
    {
        stopped = true;
        unsafe = true;
        ++failed;
        Log("FATAL " + message);
    }

    int Runner::Summary()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (callbackFailure && !stopped)
            {
                ++failed;
                stopped = true;
                Log(std::string("FATAL callback_error=") + callbackFailure);
            }
        }
        Log("SUMMARY passed=" + std::to_string(passed) + " failed=" + std::to_string(failed)
            + " skipped=" + std::to_string(skipped) + " callbacks=" + std::to_string(callbacks)
            + " connection_events=" + std::to_string(events) + " result=" + (failed ? "FAIL" : "PASS"));
        return failed ? 1 : 0;
    }

    void Runner::Health()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (callbackFailure)
        {
            unsafe = true;
            throw Failure(callbackFailure, true);
        }
    }

    bool Runner::SafeToRelease()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return !unsafe && !callbackFailure && pendingCount == 0 && !stringPending;
    }

    void Runner::RequireTransport(ClientResult result, const std::string &operation)
    {
        if (result == CLIENT_OK)
            return;
        unsafe = true;
        throw Failure(operation + ": " + Name(result) + "; remote completion is not assumed", true);
    }

    RpcResponse Runner::Decode(
        Connection &connection, RpcService service, const uint8_t *data, size_t size, const char *delivery, int slot)
    {
        RpcResponse rpc;
        if (!data || size > INT_MAX || !rpc.ParseFromArray(data, static_cast<int>(size)) || rpc.request_id() == 0
            || rpc.schema_version() != 1 || rpc.service() != service || !RpcStatus_IsValid(rpc.status())
            || rpc.status() == RPC_STATUS_UNSPECIFIED)
        {
            unsafe = true;
            throw Failure("invalid correlated RPC response on " + connection.Label(), true);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!connection.responseIds.insert(rpc.request_id()).second)
            {
                unsafe = true;
                throw Failure("response ID delivered twice on " + connection.Label(), true);
            }
        }
        std::string detail = "REPLY connection=" + connection.Label() + " mode=" + delivery;
        if (slot >= 0)
            detail += " slot=" + std::to_string(slot);
        detail += " id=" + std::to_string(rpc.request_id()) + " rpc=" + RpcStatus_Name(rpc.status());
        CryptoResponse crypto;
        if (rpc.status() == RPC_STATUS_OK && crypto.ParseFromString(rpc.payload()))
            detail += " crypto=" + CryptoError_Name(crypto.error());
        Log(detail);
        return rpc;
    }

    RpcResponse Runner::Sync(Connection &connection, const RpcRequest &request)
    {
        Health();
        const auto wire = request.SerializeAsString();
        uint8_t *data = nullptr;
        size_t size = 0;
        const auto result = client_send_request(
            connection.Get(), reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), &data, &size);
        ResponseStorage storage(data);
        Log("SEND connection=" + connection.Label() + " mode=sync local=" + Name(result));
        RequireTransport(result, "synchronous request");
        return Decode(connection, request.service(), data, size, "sync");
    }

    RpcResponse Runner::Send(Connection &connection, const CryptoRequest &request, Delivery delivery)
    {
        Log("CALL operation=" + CryptoOperation_Name(request.operation()) + " mode=" + Name(delivery));
        if (delivery == Delivery::Sync)
            return Sync(connection, Envelope(request));
        Start({ { &connection, Envelope(request) } });
        return Finish().front();
    }

    template <size_t Index> void Runner::Binary(Client *client, uint8_t *data, size_t size)
    {
        activeRunner->Receive(Index, client, data, size);
    }

    void Runner::Receive(size_t slot, Client *client, uint8_t *data, size_t size)
    {
        // No parsing, logging, allocation or request API call on the I/O thread.
        std::lock_guard<std::mutex> lock(mutex);
        auto &record = pending[slot];
        ++callbacks;
        if (!record.active || record.received || !record.connection || record.connection->Get() != client)
        {
            callbackFailure = "unexpected, duplicate or wrong-connection binary callback";
            client_free_response(data);
        }
        else
        {
            record.data = data;
            record.size = size;
            record.received = true;
        }
        changed.notify_all();
    }

    void Runner::Start(const std::vector<Call> &calls, bool concurrent)
    {
        static const auto handlers = Handlers(std::make_index_sequence<AsyncSlots> {});
        Health();
        Check(!calls.empty() && calls.size() <= AsyncSlots, "invalid diagnostic batch size");
        {
            std::lock_guard<std::mutex> lock(mutex);
            Check(pendingCount == 0, "previous diagnostic batch has not been collected");
            pendingCount = calls.size();
            for (size_t i = 0; i < calls.size(); ++i)
            {
                pending[i] = {};
                pending[i].connection = calls[i].connection;
                pending[i].service = calls[i].request.service();
                pending[i].active = true;
            }
        }
        // Different fixed callback functions provide per-request context without
        // extending EyilClient's public response-only callback signature.
        const auto submit = [&](size_t i)
        {
            const auto wire = calls[i].request.SerializeAsString();
            const auto result = client_send_request_async(
                calls[i].connection->Get(), reinterpret_cast<const uint8_t *>(wire.data()), wire.size(), handlers[i]);
            Log("SEND connection=" + calls[i].connection->Label() + " mode=async slot=" + std::to_string(i)
                + " local=" + Name(result));
            return result;
        };
        std::vector<ClientResult> results(calls.size());
        if (concurrent)
        {
            std::vector<std::future<ClientResult>> sends;
            for (size_t i = 0; i < calls.size(); ++i)
                sends.push_back(std::async(std::launch::async, submit, i));
            for (size_t i = 0; i < sends.size(); ++i)
                results[i] = sends[i].get();
        }
        else
            for (size_t i = 0; i < calls.size(); ++i)
                results[i] = submit(i);
        for (auto result : results)
            RequireTransport(result, "asynchronous submission");
    }

    std::vector<RpcResponse> Runner::Finish()
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, ResponseWait,
                [&]
                {
                    if (callbackFailure)
                        return true;
                    for (size_t i = 0; i < pendingCount; ++i)
                        if (!pending[i].received)
                            return false;
                    return true;
                }))
        {
            unsafe = true;
            throw Failure("diagnostic async wait exceeded 30 s; no remote cancellation or buffer reuse assumed", true);
        }
        if (callbackFailure)
        {
            unsafe = true;
            throw Failure(callbackFailure, true);
        }
        const auto count = pendingCount;
        for (size_t i = 0; i < count; ++i)
            pending[i].active = false;
        pendingCount = 0;
        lock.unlock();
        std::vector<RpcResponse> responses;
        for (size_t i = 0; i < count; ++i)
        {
            ResponseStorage storage(pending[i].data);
            pending[i].data = nullptr;
            responses.push_back(Decode(*pending[i].connection, pending[i].service, storage.get(), pending[i].size,
                "async", static_cast<int>(i)));
        }
        return responses;
    }

    void Runner::String(Client *client, char *data)
    {
        auto &runner = *activeRunner;
        std::lock_guard<std::mutex> lock(runner.mutex);
        ++runner.callbacks;
        if (!runner.stringPending || runner.stringReceived || runner.stringClient != client)
        {
            runner.callbackFailure = "unexpected, duplicate or wrong-connection string callback";
            client_free_response(data);
        }
        else
        {
            runner.stringData = data;
            runner.stringReceived = true;
        }
        runner.changed.notify_all();
    }

    std::string Runner::Text(Connection &connection, const char *request, Delivery delivery)
    {
        char *data = nullptr;
        if (delivery == Delivery::Sync)
        {
            const auto result = client_send_request_string(connection.Get(), request, &data);
            ResponseStorage storage(reinterpret_cast<uint8_t *>(data));
            Log("SEND connection=" + connection.Label() + " mode=string-sync local=" + Name(result));
            RequireTransport(result, "string request");
            Check(data != nullptr, "missing string response");
            return data;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            stringClient = connection.Get();
            stringPending = true;
            stringReceived = false;
        }
        const auto result = client_send_request_string_async(connection.Get(), request, String);
        Log("SEND connection=" + connection.Label() + " mode=string-async local=" + Name(result));
        RequireTransport(result, "string asynchronous submission");
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, ResponseWait, [&] { return stringReceived || callbackFailure; }) || callbackFailure)
        {
            unsafe = true;
            throw Failure(callbackFailure ? callbackFailure : "diagnostic string response wait exceeded 30 s", true);
        }
        ResponseStorage storage(reinterpret_cast<uint8_t *>(stringData));
        data = stringData;
        stringData = nullptr;
        stringPending = false;
        lock.unlock();
        Check(data != nullptr, "missing asynchronous string response");
        return data;
    }

    void Runner::Event(Client *, ClientConnectionEvent event)
    {
        auto &runner = *activeRunner;
        std::lock_guard<std::mutex> lock(runner.mutex);
        ++runner.events;
        switch (event)
        {
        case CLIENT_CONNECTION_LOST:
            runner.callbackFailure = "unexpected CLIENT_CONNECTION_LOST";
            break;
        case CLIENT_CONNECTION_RESTORED:
            runner.callbackFailure = "unexpected CLIENT_CONNECTION_RESTORED";
            break;
        case CLIENT_CONNECTION_TERMINATED:
            runner.callbackFailure = "unexpected CLIENT_CONNECTION_TERMINATED";
            break;
        default:
            runner.callbackFailure = "unknown connection event";
            break;
        }
        runner.changed.notify_all();
    }

    void Runner::Notification(Client *, uint8_t *data, size_t)
    {
        client_free_response(data);
        auto &runner = *activeRunner;
        std::lock_guard<std::mutex> lock(runner.mutex);
        ++runner.notifications;
        runner.callbackFailure = "reserved notification callback was invoked";
        runner.changed.notify_all();
    }

    void ExpectRpc(const RpcResponse &response, RpcStatus expected)
    {
        Check(response.status() == expected,
            "expected " + RpcStatus_Name(expected) + ", actual " + RpcStatus_Name(response.status()));
        if (expected != RPC_STATUS_OK)
            Check(response.payload().empty(), "Gateway error response must have empty service payload");
    }

    CryptoResponse Expect(const RpcResponse &response, CryptoOperation operation, CryptoError error)
    {
        ExpectRpc(response, RPC_STATUS_OK);
        CryptoResponse crypto;
        Check(crypto.ParseFromString(response.payload()), "cannot decode CryptoResponse");
        Check(static_cast<int>(crypto.payload_case()) == static_cast<int>(operation) + 1,
            "wrong response oneof for " + CryptoOperation_Name(operation));
        Check(crypto.error() == error,
            "expected " + CryptoError_Name(error) + ", actual " + CryptoError_Name(crypto.error()));
        return crypto;
    }

    Buffer::Buffer(Runner &runner, Connection &connection, uint32_t requested, Delivery delivery)
        : runner(runner)
        , connection(connection)
    {
        const auto response = Expect(
            runner.Send(connection, AllocateRequest(requested), delivery), CRYPTO_OPERATION_ALLOCATE_SHARED_BUFFER);
        const auto &allocation = response.allocate_shared_buffer();
        id = allocation.buffer_id();
        name = allocation.name();
        size = allocation.size();
        Check(!id.empty() && !name.empty() && name.front() == '/' && size == requested,
            "invalid allocation identity or returned size");
    }

    Buffer::~Buffer()
    {
        Unmap();
        if (!id.empty() && connection.Get() && runner.SafeToRelease())
        {
            try
            {
                Release();
            }
            catch (const std::exception &error)
            {
                runner.Abort(std::string("buffer cleanup failed: ") + error.what());
            }
        }
    }

    void Buffer::Map()
    {
        fd = shm_open(name.c_str(), O_RDWR | O_CLOEXEC, 0);
        Check(fd >= 0, "shm_open failed errno=" + std::to_string(errno));

        struct stat status
        {
        };

        Check(fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_size == size,
            "mapped object type/size differs from allocation");
        void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        Check(mapping != MAP_FAILED, "mmap failed errno=" + std::to_string(errno));
        data = static_cast<uint8_t *>(mapping);
    }

    void Buffer::Unmap()
    {
        if (data)
        {
            munmap(data, size);
            data = nullptr;
        }
        if (fd >= 0)
        {
            close(fd);
            fd = -1;
        }
    }

    void Buffer::Release(Delivery delivery)
    {
        Unmap();
        Expect(runner.Send(connection, ReleaseRequest(id), delivery), CRYPTO_OPERATION_RELEASE_SHARED_BUFFER);
        id.clear();
    }
}
