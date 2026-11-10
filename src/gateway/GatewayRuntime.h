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

#include "CryptoServiceAdapter.h"
#include "TransportIdentity.h"

#include <core/core.h>
#include <condition_variable>
#include <thread>

namespace Eyil
{
    class GatewayRuntime
    {
    public:
        explicit GatewayRuntime(CryptoServiceAdapter::Resolver resolver, unsigned workers = 4,
            std::string endpoint = Endpoint, Diagnostics::Sink sink = nullptr, std::string temporaryDirectory = {});
        ~GatewayRuntime();

        bool Start();
        void StopAndWait();

        Diagnostics &Log() { return log; }

        SharedBuffers &Buffers() { return buffers; }

    private:
        enum class Kind
        {
            Authenticate,
            Request,
            Cleanup
        };

        struct Work
        {
            explicit Work(Kind kind, std::shared_ptr<Connection> connection)
                : kind(kind)
                , connection(std::move(connection))
            {
            }

            Kind kind;
            bool tracked = false;
            std::shared_ptr<Connection> connection;
            RpcRequest request;
            std::string route;
            std::string response;
            Clock::time_point deadline;
            std::shared_ptr<Work> next;
        };

        class Job : public WPEFramework::Core::IDispatch
        {
        public:
            Job(GatewayRuntime *runtime, std::shared_ptr<Work> work)
                : runtime(runtime)
                , work(std::move(work))
            {
            }

            void Dispatch() override { runtime->Execute(work); }

        private:
            GatewayRuntime *runtime;
            std::shared_ptr<Work> work;
        };

        class Dispatcher : public WPEFramework::Core::ThreadPool::IDispatcher
        {
            void Initialize() override { }

            void Deinitialize() override { }

            void Dispatch(WPEFramework::Core::IDispatch *job) override { job->Dispatch(); }
        };

        bool Schedule(const std::shared_ptr<Work> &work); // registry mutex held
        void Execute(const std::shared_ptr<Work> &work) noexcept;

        void Io() noexcept;
        void Authenticate() noexcept;
        void Monitor() noexcept;

        bool Receive(void *router);
        bool Send(void *router, const Work &work);
        void Respond(Work &work, RpcStatus status, const std::string &payload = {}) noexcept;

        void Close(const std::shared_ptr<Connection> &connection); // registry mutex held
        void Fatal(const char *reason) noexcept;
        void Lifecycle();

        Diagnostics log;
        SharedBuffers buffers { log };
        CryptoServiceAdapter adapter;

        unsigned workers;
        std::string endpoint;
        std::string temporaryDirectory;

        Wakeup wake;
        std::unique_ptr<Context> context;
        Dispatcher dispatcher;
        std::unique_ptr<WPEFramework::Core::ThreadPool> pool;

        std::mutex mutex;
        std::mutex stopMutex;
        std::condition_variable changed;
        std::thread io;
        std::thread zap;
        std::thread monitor;

        bool started = false;
        bool stopping = false;
        bool auxiliaryStop = false;
        bool failed = false;

        bool zapReady = false;
        bool monitorReady = false;
        bool monitorEnabled = false;
        bool listenerReady = false;

        unsigned reserved = 0;
        std::unordered_map<uint64_t, std::shared_ptr<Connection>> connections;
        std::shared_ptr<Work> current;
        std::shared_ptr<Work> completedHead;
        std::shared_ptr<Work> completedTail;
    };
}
