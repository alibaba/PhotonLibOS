/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <photon/common/object.h>
#include <photon/common/callback.h>
#include <photon/common/iovector.h>
#include <photon/net/socket.h>
#include <photon/net/http/message.h>
#include <photon/net/http/server.h>
#include <photon/net/http/client.h>
#include <string>
#include <string_view>

namespace photon {
namespace net {
namespace http {

// WebSocket frame opcodes as defined in RFC 6455
enum class WebSocketOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

// WebSocket close codes as defined in RFC 6455
enum class WebSocketCloseCode : uint16_t {
    NormalClosure = 1000,
    GoingAway = 1001,
    ProtocolError = 1002,
    UnsupportedDataType = 1003,
    NoStatusRcvd = 1005,
    AbnormalClosure = 1006,
    InvalidFramePayloadData = 1007,
    PolicyViolation = 1008,
    MessageTooBig = 1009,
    MandatoryExt = 1010,
    InternalServerError = 1011,
    TLSHandshake = 1015
};

/**
 * @brief WebSocket connection interface for both client and server sides
 */
class IWebSocketStream : public Object {
public:
    virtual ~IWebSocketStream() = default;
    
    /**
     * @brief Send text data to the WebSocket
     * @param text Text data to send
     * @param timeout Timeout in microseconds (-1 for infinite)
     * @return Number of bytes sent, or -1 on error
     */
    virtual ssize_t send_text(std::string_view text, uint64_t timeout = -1) = 0;
    
    /**
     * @brief Send binary data to the WebSocket (contiguous buffer)
     * @param data Binary data to send
     * @param size Size of binary data
     * @param timeout Timeout in microseconds (-1 for infinite)
     * @return Number of bytes sent, or -1 on error
     */
    virtual ssize_t send_binary(const void* data, size_t size, uint64_t timeout = -1) = 0;
    
    /**
     * @brief Send binary data to the WebSocket (scatter-gather I/O)
     * @param iov IOVector containing data to send
     * @param timeout Timeout in microseconds (-1 for infinite)
     * @return Number of bytes sent, or -1 on error
     */
    virtual ssize_t send_binary(iovector_view iov, uint64_t timeout = -1) = 0;
    
    /**
     * @brief Send a ping frame
     * @param data Optional ping payload
     * @param timeout Timeout in microseconds (-1 for infinite)
     * @return 0 on success, -1 on error
     */
    virtual int ping(std::string_view data = "", uint64_t timeout = -1) = 0;
    
    /**
     * @brief Receive frame data from the WebSocket (contiguous buffer)
     * @param buf Buffer to store received data
     * @param size Size of buffer
     * @param opcode Output parameter for frame opcode
     * @param timeout Timeout in microseconds (-1 for infinite)
     * @return Number of bytes received, or -1 on error
     */
    virtual ssize_t recv_frame(void* buf, size_t size, WebSocketOpcode* opcode = nullptr, uint64_t timeout = -1) = 0;
    
    /**
     * @brief Receive frame data from the WebSocket (scatter-gather I/O)
     * @param iov IOVector to store received data
     * @param opcode Output parameter for frame opcode
     * @param timeout Timeout in microseconds (-1 for infinite)
     * @return Number of bytes received, or -1 on error
     */
    virtual ssize_t recv_frame(iovector* iov, WebSocketOpcode* opcode = nullptr, uint64_t timeout = -1) = 0;
    
    /**
     * @brief Close the WebSocket connection
     * @param code Close code (default NormalClosure)
     * @param reason Optional reason text
     * @return 0 on success, -1 on error
     */
    virtual int close(WebSocketCloseCode code = WebSocketCloseCode::NormalClosure, std::string_view reason = "") = 0;
    
    /**
     * @brief Check if the WebSocket connection is closed
     * @return True if closed, false otherwise
     */
    virtual bool is_closed() const = 0;
    
    /**
     * @brief Get the underlying socket stream
     * @return Pointer to the underlying ISocketStream
     */
    virtual ISocketStream* get_socket_stream() = 0;
};

/**
 * @brief Connect to a WebSocket server
 * 
 * This function uses HTTPClient to connect to a WebSocket server and perform
 * the upgrade handshake. It sets the necessary upgrade headers and executes
 * the request.
 * 
 * @param client HTTP client to use for the connection
 * @param url Full URL for the WebSocket endpoint (e.g., "http://example.com/ws")
 * @param timeout Timeout in microseconds (-1 for infinite)
 * @return Pointer to IWebSocketStream on success, nullptr on failure
 * 
 * Example usage:
 * @code
 * auto http_client = new_http_client();
 * auto ws = websocket_connect(http_client, "http://example.com/ws");
 * if (ws) {
 *     ws->send_text("Hello");
 *     // ...
 *     delete ws;
 * }
 * delete http_client;
 * @endcode
 */
IWebSocketStream* websocket_connect(Client* client,
                                    std::string_view url,
                                    uint64_t timeout = -1);

/**
 * @brief Accept a WebSocket upgrade request on the server side
 * 
 * This function should be called from an HTTP handler when the client
 * requests a WebSocket upgrade. It validates the upgrade request and
 * performs the handshake.
 * 
 * @param req The HTTP request from the client
 * @param resp The HTTP response to send back
 * @return Pointer to IWebSocketStream on success, nullptr on failure
 * 
 * Example usage in an HTTP handler:
 * @code
 * int handle_request(Request& req, Response& resp, std::string_view) {
 *     if (req.headers["Upgrade"] == "websocket") {
 *         auto ws = server_accept_websocket(req, resp);
 *         if (ws) {
 *             // Handle WebSocket communication
 *             WebSocketOpcode opcode;
 *             char buf[4096];
 *             while (true) {
 *                 auto len = ws->recv_frame(buf, sizeof(buf), &opcode);
 *                 if (len < 0 || opcode == WebSocketOpcode::Close) break;
 *                 ws->send_text(std::string_view(buf, len)); // Echo
 *             }
 *             delete ws;
 *         }
 *         return 0;
 *     }
 *     // Handle normal HTTP request
 *     return 0;
 * }
 * @endcode
 */
IWebSocketStream* server_accept_websocket(Request& req, Response& resp);

/**
 * @brief WebSocket handler callback type for server
 * 
 * Handler receives the WebSocket stream after successful upgrade.
 * Return 0 for success, negative for error.
 */
using WebSocketHandler = Delegate<int, IWebSocketStream*>;

/**
 * @brief Create a WebSocket HTTP handler
 * 
 * This creates an HTTPHandler that automatically handles WebSocket upgrades.
 * When a WebSocket upgrade request is received, the handler performs the
 * handshake and calls the provided callback with the WebSocket stream.
 * 
 * @param handler Callback to handle WebSocket connections
 * @return HTTPHandler that can be added to HTTP server
 * 
 * Example usage:
 * @code
 * auto http_server = new_http_server();
 * auto ws_handler = new_websocket_handler([](IWebSocketStream* ws) {
 *     // Echo server
 *     char buf[4096];
 *     WebSocketOpcode opcode;
 *     while (true) {
 *         auto len = ws->recv_frame(buf, sizeof(buf), &opcode);
 *         if (len < 0 || opcode == WebSocketOpcode::Close) break;
 *         if (opcode == WebSocketOpcode::Text) {
 *             ws->send_text(std::string_view(buf, len));
 *         }
 *     }
 *     return 0;
 * });
 * http_server->add_handler(ws_handler, true, "/ws");
 * @endcode
 */
HTTPHandler* new_websocket_handler(WebSocketHandler handler);

} // namespace http
} // namespace net
} // namespace photon
