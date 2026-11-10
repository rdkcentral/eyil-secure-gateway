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

#ifndef EYIL_CLIENT_H
#define EYIL_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define EYIL_CLIENT_API __attribute__((visibility("default")))
#else
#define EYIL_CLIENT_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct Client Client;

    /*
     * @param client [IN] Client that received the notification.
     * @param notification_data [IN, ownership transferred] Library-allocated
     *        serialized Google Protocol Buffers notification message. The
     *        message type is defined by the agreed .proto schema. Ownership is
     *        transferred to the callback, which must free the data when it is no
     *        longer needed.
     * @param notification_size [IN] Number of bytes in notification_data.
     */
    typedef void (*ClientNotificationHandler)(Client *client, uint8_t *notification_data, size_t notification_size);

    /*
     * @param client [IN] Client that received the response.
     * @param response_data [IN, ownership transferred] Library-allocated response
     *        data. Ownership is transferred to the callback, which must release
     *        the data with client_free_response().
     * @param response_size [IN] Number of bytes in response_data.
     */
    typedef void (*ClientResponseHandler)(Client *client, uint8_t *response_data, size_t response_size);

    /*
     * @param client [IN] Client that received the response.
     * @param response_data [IN, ownership transferred] Library-allocated,
     *        NULL-terminated response string. Ownership is transferred to the
     *        callback, which must release the data with client_free_response().
     */
    typedef void (*ClientStringResponseHandler)(Client *client, char *response_data);

    typedef enum
    {
        CLIENT_CONNECTION_LOST, /* Terminal loss; disconnect this handle before connecting anew. */
        CLIENT_CONNECTION_RESTORED, /* Reserved; no automatic reconnect in this version. */
        CLIENT_CONNECTION_TERMINATED /* Reserved; initial authentication errors are returned by connect. */
    } ClientConnectionEvent;

    /*
     * @param client [IN] Client whose connection state changed.
     * @param event [IN] Connection state transition.
     */
    typedef void (*ClientConnectionEventHandler)(Client *client, ClientConnectionEvent event);

    typedef enum
    {
        CLIENT_OK = 0,
        CLIENT_INVALID_ARGUMENT,
        CLIENT_NOT_CONNECTED,
        CLIENT_AUTH_FAILED,
        CLIENT_TIMEOUT,
        CLIENT_COMMUNICATION_ERROR,
        CLIENT_OUT_OF_MEMORY,
        CLIENT_ALREADY_CONNECTED,
        CLIENT_NOT_IMPLEMENTED
    } ClientResult;

    /**
     * Connects a client. Authentication uses the process credentials supplied by
     * the transport; callers do not provide ZAP credentials.
     *
     * Waits up to 15 seconds for handshake readiness. Initial success/failure
     * has no connection callback. Later unexpected loss emits LOST once;
     * explicit disconnect has no connection callback.
     * @param client [IN/OUT] NULL-initialized handle slot. An occupied live slot
     *        returns CLIENT_ALREADY_CONNECTED without modifying its handle.
     *        A new handle is published only on CLIENT_OK.
     * @param handler [IN] Connection event callback. It runs on an internal
     *        thread and must not call other client APIs. It is automatically
     *        unregistered by client_disconnect().
     *
     * @return CLIENT_OK on success, otherwise an appropriate ClientResult value.
     */
    EYIL_CLIENT_API ClientResult client_connect(Client **client, ClientConnectionEventHandler handler);

    /**
     * Sends a serialized Google Protocol Buffers request and returns the
     * serialized response buffer for the caller. The request and response use
     * the protobuf wire format and must be interpreted using the agreed message
     * schema. The caller supplies RpcRequest(service, opaque payload); the
     * library replaces request_id/schema_version and validates RpcResponse.
     * One 15-second budget includes submission and response delivery. A timeout
     * does not cancel backend work. Gateway error responses still return CLIENT_OK.
     *
     * @param client [IN] Connected client handle.
     * @param request_data [IN] Serialized protobuf request message. Must contain
     *        exactly request_size bytes and must not be NULL when request_size is
     *        non-zero.
     * @param request_size [IN] Size in bytes of the serialized protobuf request.
     * @param response_data [OUT] Receives a library-allocated serialized protobuf
     *        response message on CLIENT_OK. Set to NULL for every other result.
     *        The caller must release a successful response with
     *        client_free_response().
     * @param response_size [OUT] Receives the size in bytes of the serialized
     *        protobuf response. Successful calls always return a non-zero size;
     *        every local error sets this output to zero.
     *
     * @return CLIENT_OK on success, otherwise an appropriate ClientResult value.
     */
    EYIL_CLIENT_API ClientResult client_send_request(Client *client, const uint8_t *request_data, size_t request_size,
        uint8_t **response_data, size_t *response_size);

    /**
     * Sends a serialized Google Protocol Buffers request through the same
     * transport and validation as client_send_request(). Waits for send
     * acceptance, then returns without waiting for the response. Submission has
     * a 15-second deadline; response waiting has no timeout.
     *
     * @param client [IN] Connected client handle.
     * @param request_data [IN] Serialized protobuf request message. Must contain
     *        exactly request_size bytes and must not be NULL when request_size is
     *        non-zero.
     * @param request_size [IN] Size in bytes of the serialized protobuf request.
     * @param handler [IN] Non-NULL callback receiving a validated response at
     *        most once. Response ownership passes to the callback, which must
     *        release it with client_free_response(). Callbacks run on this
     *        Client's I/O thread without internal locks, possibly before the
     *        submitting thread resumes. Request APIs return CLIENT_INVALID_ARGUMENT
     *        on that thread; schedule further requests/disconnect on an app thread.
     *        A post-send local failure or disconnect retires the request without
     *        a callback. No NULL/synthetic error response is generated.
     *
     * @return CLIENT_OK after libzmq accepts the message, otherwise an
     *         appropriate ClientResult value. An error return invokes no
     *         response callback. CLIENT_OK confirms local send acceptance;
     *         it does not guarantee remote execution or response delivery.
     */
    EYIL_CLIENT_API ClientResult client_send_request_async(
        Client *client, const uint8_t *request_data, size_t request_size, ClientResponseHandler handler);

    /**
     * Sends a NULL-terminated request string and returns a NULL-terminated
     * response string. Copies bytes up to the first NUL; no Base64/ProtoJSON.
     * The current Gateway rejects service=0 with RPC_STATUS_UNSUPPORTED_SERVICE;
     * successful transport returns CLIENT_OK and that exact status name as text.
     * This does not select a Crypto operation. The synchronous budget is 15 seconds.
     *
     * @param client [IN] Connected client handle.
     * @param request_data [IN] NULL-terminated request string.
     * @param response_data [OUT] Receives a library-allocated, NULL-terminated
     *        response string on CLIENT_OK. Set to NULL for every other result.
     *        The caller must release it with client_free_response().
     *
     * @return CLIENT_OK on success, otherwise an appropriate ClientResult value.
     */
    EYIL_CLIENT_API ClientResult client_send_request_string(
        Client *client, const char *request_data, char **response_data);

    /**
     * Sends a NULL-terminated request string through the same transport and
     * validation as client_send_request_string(). Waits for send acceptance,
     * then returns without waiting for the response. Submission has a
     * 15-second deadline; response waiting has no timeout.
     *
     * @param client [IN] Connected client handle.
     * @param request_data [IN] NULL-terminated request string.
     * @param handler [IN] Non-NULL callback receiving a validated response
     *        string at most once. Response ownership passes to the callback,
     *        which must release it with client_free_response().
     *
     * @return CLIENT_OK after libzmq accepts the message, otherwise an
     *         appropriate ClientResult value. An error return invokes no
     *         response callback. CLIENT_OK confirms local send acceptance;
     *         it does not guarantee remote execution or response delivery.
     */
    EYIL_CLIENT_API ClientResult client_send_request_string_async(
        Client *client, const char *request_data, ClientStringResponseHandler handler);

    /**
     * Frees a response allocated by a synchronous request API or passed to a
     * response handler.
     *
     * @param response_data [IN] Response buffer, or NULL. Passing NULL is safe.
     */
    EYIL_CLIENT_API void client_free_response(void *response_data);

    /**
     * Reserved for future notifications. Always returns CLIENT_NOT_IMPLEMENTED;
     * no handler is stored or invoked and no subscription is created.
     *
     * @param client [IN] Connected client handle.
     * @param handler [IN] Ignored reserved callback.
     *
     * @return CLIENT_NOT_IMPLEMENTED.
     */
    EYIL_CLIENT_API ClientResult client_register_notifications(Client *client, ClientNotificationHandler handler);

    /**
     * Unregisters notifications.
     *
     * @param client [IN] Client whose notifications are registered.
     *
     * No effect in this version; no allocation or callback state is retained.
     */
    EYIL_CLIENT_API void client_unregister_notifications(Client *client);

    /**
     * Stops admission, wakes pending sync/send waiters with CLIENT_NOT_CONNECTED,
     * retires async response records, joins I/O and quiesces callbacks/callers.
     * Does not emit a connection callback or cancel remote execution.
     * Call on an application thread, never from a callback. The caller must
     * prevent new calls and clear its own handle slot after this function returns.
     * @param client [IN] Client handle to release, or NULL.
     */
    EYIL_CLIENT_API void client_disconnect(Client *client);

#ifdef __cplusplus
}
#endif

#endif
