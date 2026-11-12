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

#include <ProtocolVersion.h>
#include <crypto.pb.h>
#include <rpc.pb.h>

#include <cstddef>

extern "C"
{
    __attribute__((visibility("default"))) const void *eyil_consumer_rpc_descriptor()
    {
        return RpcRequest::descriptor();
    }

    __attribute__((visibility("default"))) const void *eyil_consumer_crypto_descriptor()
    {
        return CryptoRequest::descriptor();
    }

    __attribute__((visibility("default"))) bool eyil_consumer_decode(const char *data, size_t size)
    {
        RpcRequest request;
        CryptoRequest crypto;
        return request.ParseFromArray(data, static_cast<int>(size)) && request.request_id() == 42
            && request.service() == RPC_SERVICE_CRYPTO && request.schema_version() == EYIL_RPC_SCHEMA_VERSION
            && crypto.ParseFromString(request.payload()) && crypto.operation() == CRYPTO_OPERATION_FLUSH_KEYS
            && crypto.has_flush_keys();
    }
}
