// Copyright 2026 RDK Management
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <eyilclient/EyilClient.h>
#include <crypto.pb.h>
#include <rpc.pb.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace EyilDev
{
    constexpr uint32_t MaximumBufferSize = 1572864;
    constexpr size_t AsyncSlots = 16;

    enum class Mode
    {
        Exercise,
        TestBackend,
        MissingBackend
    };
    enum class Delivery
    {
        Sync,
        Async
    };

    class Failure : public std::runtime_error
    {
    public:
        Failure(const std::string &message, bool fatal = false)
            : std::runtime_error(message)
            , fatal(fatal)
        {
        }

        const bool fatal;
    };

    void Check(bool condition, const std::string &message);
    const char *Name(ClientResult result);
    const char *Name(Delivery delivery);
    RpcRequest Envelope(const CryptoRequest &request);
    CryptoRequest Request(CryptoOperation operation, const std::string &bufferId = {});
    CryptoRequest AllocateRequest(uint32_t size);
    CryptoRequest ReleaseRequest(const std::string &id);
    CryptoRequest RandomRequest(uint32_t size);
    BufferRegion Region(uint32_t offset, uint32_t length);

    class Runner;

    class Connection
    {
    public:
        Connection(Runner &runner, std::string label);
        ~Connection();
        Connection(const Connection &) = delete;
        Connection &operator=(const Connection &) = delete;

        void Connect();
        void Close();

        Client *Get() const { return client; }

        const std::string &Label() const { return label; }

    private:
        friend class Runner;
        Runner &runner;
        std::string label;
        Client *client = nullptr;
        std::set<uint64_t> responseIds;
    };

    struct Call
    {
        Connection *connection;
        RpcRequest request;
    };

    class Runner
    {
    public:
        explicit Runner(Mode mode);
        ~Runner();
        Runner(const Runner &) = delete;
        Runner &operator=(const Runner &) = delete;

        void Run(const std::string &name, const std::string &expected, const std::function<void()> &test);
        void Skip(const std::string &name, const std::string &reason);
        void Log(const std::string &message);
        void Abort(const std::string &message);
        int Summary();
        void Health();
        bool SafeToRelease();

        Mode GetMode() const { return mode; }

        bool Stopped() const { return stopped; }

        RpcResponse Sync(Connection &connection, const RpcRequest &request);
        RpcResponse Send(Connection &connection, const CryptoRequest &request, Delivery delivery);
        void Start(const std::vector<Call> &calls, bool concurrent = false);
        std::vector<RpcResponse> Finish();
        std::string Text(Connection &connection, const char *request, Delivery delivery);

        static void Event(Client *client, ClientConnectionEvent event);
        static void Notification(Client *, uint8_t *data, size_t);

    private:
        struct Pending
        {
            Connection *connection = nullptr;
            RpcService service = RPC_SERVICE_UNSPECIFIED;
            uint8_t *data = nullptr;
            size_t size = 0;
            bool active = false;
            bool received = false;
        };

        template <size_t Index> static void Binary(Client *client, uint8_t *data, size_t size);

        template <size_t... Index> static auto Handlers(std::index_sequence<Index...>)
        {
            return std::array<ClientResponseHandler, sizeof...(Index)> { &Binary<Index>... };
        }

        static void String(Client *client, char *data);
        void Receive(size_t slot, Client *client, uint8_t *data, size_t size);
        RpcResponse Decode(Connection &connection, RpcService service, const uint8_t *data, size_t size,
            const char *delivery, int slot = -1);
        void RequireTransport(ClientResult result, const std::string &operation);

        static Runner *activeRunner;
        Mode mode;
        std::mutex mutex;
        std::condition_variable changed;
        std::mutex outputMutex;
        std::array<Pending, AsyncSlots> pending;
        size_t pendingCount = 0;
        char *stringData = nullptr;
        Client *stringClient = nullptr;
        bool stringPending = false;
        bool stringReceived = false;
        const char *callbackFailure = nullptr;
        bool unsafe = false;
        bool stopped = false;
        unsigned passed = 0;
        unsigned failed = 0;
        unsigned skipped = 0;
        unsigned events = 0;
        unsigned notifications = 0;
        unsigned callbacks = 0;
    };

    CryptoResponse Expect(
        const RpcResponse &response, CryptoOperation operation, CryptoError error = CRYPTO_ERROR_NONE);
    void ExpectRpc(const RpcResponse &response, RpcStatus status);
    void CheckRandom(Runner &runner, const RpcResponse &response, uint32_t size);

    // This helper owns only the application's mapping. Gateway owns unlink and
    // releases the allocation through the normal RPC or disconnect lifecycle.
    class Buffer
    {
    public:
        Buffer(Runner &runner, Connection &connection, uint32_t size = 4096, Delivery delivery = Delivery::Sync);
        ~Buffer();
        Buffer(const Buffer &) = delete;
        Buffer &operator=(const Buffer &) = delete;

        void Map();
        void Unmap();
        void Release(Delivery delivery = Delivery::Sync);

        const std::string &Id() const { return id; }

        const std::string &ObjectName() const { return name; }

        uint32_t Size() const { return size; }

        uint8_t *Data() const { return data; }

        Connection &Owner() const { return connection; }

    private:
        Runner &runner;
        Connection &connection;
        std::string id;
        std::string name;
        uint32_t size = 0;
        int fd = -1;
        uint8_t *data = nullptr;
    };

    void ApiCases(Runner &runner, Connection &a);
    void KeyCases(Runner &runner, Connection &a);
    void ConcurrentCases(Runner &runner, Connection &a, Connection &b);
    void BulkCases(Runner &runner, Connection &a, Connection &b);
    void AllocationCases(Runner &runner, Connection &a, Connection &b);
    void OwnershipCases(Runner &runner, Connection &a, Connection &b);
    void RegionCases(Runner &runner, Connection &a);
    void InvalidRequestCases(Runner &runner, Connection &a);
    void LifecycleCases(Runner &runner, Connection &a, Connection &b);
}
