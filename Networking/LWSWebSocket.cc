//
// LWSWebSocket.cc
//
// Copyright (c) 2019 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "LWSWebSocket.hh"
#include "LWSContext.hh"
#include "LWSServer.hh"
#include "LWSUtil.hh"
#include "c4LibWebSocketFactory.h"
#include "c4ExceptionUtils.hh"
#include "Certificate.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "libwebsockets.h"

#include <deque>
#include <mutex>
#include <thread>

using namespace std;
using namespace fleece;


#define LWS_WRITE_CLOSE lws_write_protocol(4)


namespace litecore { namespace net {

    // Max number of bytes read that haven't been handled by the replicator yet.
    // Beyond this point, we turn on backpressure (flow-control) in libwebsockets
    // so it stops reqding the socket.
    static constexpr size_t kMaxUnreadBytes = 100 * 1024;


    LWSWebSocket::LWSWebSocket(lws *client, C4Socket* s)
    :LWSProtocol(client)
    {
        setC4Socket(s);
    }


    LWSClientWebSocket::LWSClientWebSocket(C4Socket *socket, const C4Address &to, const AllocedDict &options)
    :LWSWebSocket(nullptr, socket)
    ,_address(to)
    ,_options(options)
    { }


    LWSServerWebSocket::LWSServerWebSocket(lws *client, LWSServer *server)
    :LWSWebSocket(client, nullptr)
    {
        // Get peer's IP address:
        C4Address peerAddress = server->address();
        char ipBuf[100];
        lws_get_peer_simple(_client, ipBuf, sizeof(ipBuf)-1);
        peerAddress.hostname = slice(ipBuf);

        // Create C4Socket attached to me now:
        auto factory = C4LWSWebSocketFactory;
        factory.open = nullptr;
        setC4Socket( c4socket_fromNative(factory, this, &peerAddress) );

        LogVerbose("Created %p on wsi %p,  C4Socket %p", this, client, _c4socket);
    }


    LWSServerWebSocket::~LWSServerWebSocket() {
        C4LogToAt(kC4WebSocketLog, kC4LogDebug, "DESTRUCT LWSServerWebSocket %p", this);
    }


    void LWSServerWebSocket::upgraded() {
        lws_set_opaque_user_data(_client, this);
    }


    void LWSServerWebSocket::canceled() {
        closeC4Socket(LiteCoreDomain, kC4ErrorUnexpectedError, nullslice);
        _client = nullptr;
        release(this);
    }


    void LWSWebSocket::setC4Socket(C4Socket* s) {
        if (s) {
            Assert(!_c4socket);
            retain(this);   // makes nativeHandle a strong reference
            _c4socket = s;
            _c4socket->nativeHandle = this;
        } else if (_c4socket) {
            _c4socket->nativeHandle = nullptr;
            _c4socket = nullptr;
            release(this);
        }
    }


#pragma mark - C4SOCKET CALLBACKS:


    static inline LWSWebSocket* internal(C4Socket *sock) {
        return ((LWSWebSocket*)sock->nativeHandle);
    }


    void LWSClientWebSocket::sock_open(C4Socket *sock, const C4Address *c4To, FLSlice optionsFleece, void*) {
        auto self = new LWSClientWebSocket(sock, *c4To, AllocedDict((slice)optionsFleece));
        self->open();
    }


    void LWSWebSocket::sock_write(C4Socket *sock, C4SliceResult allocatedData) {
        if (internal(sock))
            internal(sock)->write(alloc_slice(move(allocatedData)));
    }

    void LWSWebSocket::sock_completedReceive(C4Socket *sock, size_t byteCount) {
        if (internal(sock))
            internal(sock)->completedReceive(byteCount);
    }


    void LWSWebSocket::sock_requestClose(C4Socket *sock, int status, C4String message) {
        if (internal(sock))
            internal(sock)->requestClose(status, message);
    }


    void LWSWebSocket::sock_dispose(C4Socket *sock) {
        if (internal(sock))
            internal(sock)->setC4Socket(nullptr);
    }


    void LWSClientWebSocket::open() {
        Assert(!_client);
        Log("LWSWebSocket connecting to <%.*s>...", SPLAT(_address.url()));
        slice pinnedCert = _options[kC4ReplicatorOptionPinnedServerCert].asData();
        LWSContext::instance().connectClient(this, LWSContext::kBLIPClientProtocol,
                                             _address, pinnedCert);
        _c4socket->nativeHandle = this;
    }


    void LWSWebSocket::write(const alloc_slice &message) {
        LogDebug("Queuing send of %zu byte message", message.size);
        _sendFrame(LWS_WRITE_BINARY, LWS_CLOSE_STATUS_NOSTATUS, message);
    }


    void LWSWebSocket::requestClose(int status, slice message) {
        Log("Closing with WebSocket status %d '%.*s'", status, SPLAT(message));
        _sendFrame(LWS_WRITE_CLOSE, (lws_close_status)status, message);
    }


    void LWSWebSocket::completedReceive(size_t byteCount) {
        synchronized([&]{
            if (!_client)
                return;
            _unreadBytes -= byteCount;
            LogDebug("Completed receive of %6zd bytes  (now %6zd pending)",
                     byteCount, ssize_t(_unreadBytes));
            if (_readsThrottled && _unreadBytes <= (kMaxUnreadBytes / 2)) {
                Log("Un-throttling input (caught up)");
                _readsThrottled = false;
                lws_rx_flow_control(_client, 1 | LWS_RXFLOW_REASON_FLAG_PROCESS_NOW);
            }
        });
    }


    void LWSWebSocket::_sendFrame(uint8_t /*lws_write_protocol*/ opcode,
                                  int /*lws_close_status*/ status,
                                  slice body)
    {
        // LWS requires that a message be prefixed with LWS_PRE writeable bytes, so it can fill them
        // in with WebSocket frame headers. So pad the message.
        // Then store the opcode and status code in the padding, for use by onWriteable():
        alloc_slice frame(LWS_PRE + body.size);
        memcpy((void*)&frame[LWS_PRE], body.buf, body.size);
        (uint8_t&)frame[0] = opcode;
        memcpy((void*)&frame[1], &status, sizeof(status));

        synchronized([&]{
            if (_client) {
                _outbox.push_back(frame);
                callbackOnWriteable(); // triggers LWS_CALLBACK_CLIENT_WRITEABLE and onWriteable()
            }
        });
    }


#pragma mark - LIBWEBSOCKETS CALLBACK:


    // Dispatch events sent by libwebsockets.
    void LWSWebSocket::onEvent(lws *wsi, int reason, void *user, void *in, size_t len) {
        switch ((lws_callback_reasons)reason) {
                // Read/write:
            case LWS_CALLBACK_CLIENT_WRITEABLE:
            case LWS_CALLBACK_SERVER_WRITEABLE:
                LogDebug("**** %-s", LWSCallbackName(reason));
                onWriteable();
                break;
            case LWS_CALLBACK_CLIENT_RECEIVE:
            case LWS_CALLBACK_RECEIVE:
                onReceivedMessage(slice(in, len));
                break;

                // Close:
            case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
                LogVerbose("**** LWS_CALLBACK_WS_PEER_INITIATED_CLOSE");
                onCloseRequest(slice(in, len));
                break;
            case LWS_CALLBACK_CLIENT_CLOSED:
            case LWS_CALLBACK_CLOSED:
#if DEBUG
                LogVerbose("**** %-s", LWSCallbackName(reason));
#endif
                onClosed();
                break;

            default:
                LWSProtocol::onEvent(wsi, reason, user, in, len);
                break;
        }
    }


    void LWSClientWebSocket::onEvent(lws *wsi, int reason, void *user, void *in, size_t len) {
        switch ((lws_callback_reasons)reason) {
                // Connecting:
            case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
                LogCallback();
                onSendCustomHeaders(in, len);
                break;
            case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH:
                LogCallback();
                onConnected();
                break;
            case LWS_CALLBACK_CLIENT_ESTABLISHED:
                break;
            default:
                LWSWebSocket::onEvent(wsi, reason, user, in, len);
        }
    }


#pragma mark - HANDLERS:


    // Returns false if libwebsocket wouldn't let us write all the headers
    bool LWSClientWebSocket::onSendCustomHeaders(void *in, size_t len) {
        // "in is a char **, it's pointing to a char * which holds the
        // next location in the header buffer where you can add
        // headers, and len is the remaining space in the header buffer"
        auto dst = (uint8_t**)in;
        uint8_t *end = *dst + len;

        // Add auth header:
        Dict auth = _options[kC4ReplicatorOptionAuthentication].asDict();
        if (auth) {
            slice authType = auth[kC4ReplicatorAuthType].asString();
            if (authType == slice(kC4AuthTypeBasic)) {
                auto user = auth[kC4ReplicatorAuthUserName].asString();
                auto pass = auth[kC4ReplicatorAuthPassword].asString();
                string cred = slice(format("%.*s:%.*s", SPLAT(user), SPLAT(pass))).base64String();
                if (!addRequestHeader(dst, end, "Authorization:", slice("Basic " + cred)))
                    return false;
            } else {
                closeC4Socket(WebSocketDomain, 401,
                              "Unsupported auth type in replicator configuration"_sl);
                return false;
            }
        }

        // Add cookie header:
        slice cookies = _options[kC4ReplicatorOptionCookies].asString();
        if (cookies) {
            if (!addRequestHeader(dst, end, "Cookie:", cookies))
                return false;
        }

        // Add other custom headers:
        Dict::iterator header(_options[kC4ReplicatorOptionExtraHeaders].asDict());
        for (; header; ++header) {
            string headerStr = string(header.keyString()) + ':';
            if (!addRequestHeader(dst, end, headerStr.c_str(), header.value().asString()))
                return false;
        }
        return true;
    }


    void LWSClientWebSocket::onConnected() {
        gotResponse();
        c4socket_opened(_c4socket);
        callbackOnWriteable();
    }


    void LWSClientWebSocket::gotResponse() {
        int status = decodeHTTPStatus().first;
        if (status > 0) {
            alloc_slice headers = encodeHTTPHeaders().allocedData();
            c4socket_gotHTTPResponse(_c4socket, status, headers);
        }
    }


    void LWSWebSocket::onWriteable() {
        // Pop first message from outbox queue:
        alloc_slice msg;
        bool more;
        synchronized([&]{
            if (!_outbox.empty()) {
                msg = _outbox.front();
                _outbox.pop_front();
                more = !_outbox.empty();
            }
            LogDebug("onWriteable: %zu bytes to send; %zu msgs remaining",
                     msg.size, _outbox.size());
        });
        if (!msg)
            return;

        auto opcode = (enum lws_write_protocol) msg[0];
        slice payload = msg;
        payload.moveStart(LWS_PRE);

        if (opcode != LWS_WRITE_CLOSE) {
            // Regular WebSocket message:
            int m = lws_write(_client, (uint8_t*)payload.buf, payload.size, opcode);
            if (m < 0) {
                Log("ERROR %d writing to ws socket\n", m);
                check(m);
                return;
            }

            // Notify C4Socket that it was written:
            c4socket_completedWrite(_c4socket, payload.size);

            // Schedule another onWriteable call if there are more messages:
            if (more) {
                synchronized([&]{
                    callbackOnWriteable();
                });
            }

        } else {
            // I'm initiating closing the socket. Set the status/reason to go in the CLOSE msg:
            synchronized([&]{
                Assert(!_sentCloseFrame);
                _sentCloseFrame = true;
            });
            int status;
            memcpy(&status, &msg[1], sizeof(status));
            LogVerbose("Writing CLOSE message, status %d, msg '%.*s'", status, SPLAT(payload));
            lws_close_reason(_client, (lws_close_status)status, (uint8_t*)payload.buf, payload.size);
            setEventResult(-1); // tells libwebsockets to close the connection
        }
    }


    void LWSWebSocket::onReceivedMessage(slice data) {
        LogDebug("**** LWS_CALLBACK_CLIENT_RECEIVE  %4zd bytes  (%zd remaining)",
                 data.size, lws_remaining_packet_payload(_client));

        bool final = lws_is_final_fragment(_client);
        if (!final && !_incomingMessage) {
            // Beginning of fragmented message:
            _incomingMessage = alloc_slice(data.size + lws_remaining_packet_payload(_client));
            _incomingMessageLength = 0;
        }

        if (_incomingMessage) {
            Assert(_incomingMessageLength + data.size <= _incomingMessage.size);
            memcpy((void*)&_incomingMessage[_incomingMessageLength], data.buf, data.size);
            _incomingMessageLength += data.size;
            data = _incomingMessage;
        }

        if (final) {
            synchronized([&]{
                _unreadBytes += data.size;
                if (!_readsThrottled && _unreadBytes > kMaxUnreadBytes) {
                    Log("Throttling input (receiving too fast)");
                    _readsThrottled = true;
                    lws_rx_flow_control(_client, 0);
                }
            });
            c4socket_received(_c4socket, data);

            _incomingMessage = nullslice;
        }
    }


    // Peer initiating close. Returns true if I should send back a CLOSE message
    void LWSWebSocket::onCloseRequest(slice body) {
        // libwebsockets doc: "If you return 0 lws will echo the close and then close the
        // connection.  If you return nonzero lws will just close the
        // connection."
        // Protocol spec: https://tools.ietf.org/html/rfc6455#section-7
        LogVerbose("Received close request");
        bool sendCloseFrame;
        synchronized([&]() {
            sendCloseFrame = !_sentCloseFrame;
            _sentCloseFrame = true;
        });
        setEventResult(sendCloseFrame);
    }


    void LWSWebSocket::onConnectionError(C4Error error) {
        closeC4Socket(error);
    }


    void LWSClientWebSocket::onConnectionError(C4Error error) {
        gotResponse();
        LWSWebSocket::onConnectionError(error);
    }


    void LWSWebSocket::onDestroy() {
        synchronized([&]() {
            if (_c4socket) {
                Log("Server unexpectedly closed connection");
                closeC4Socket(NetworkDomain, kC4NetErrUnknown,
                              "Server unexpectedly closed socket"_sl);
            }
        });
    }


    void LWSWebSocket::onClosed() {
        synchronized([&]() {
            if (_sentCloseFrame) {
                Log("Connection closed");
                closeC4Socket(WebSocketDomain, kWebSocketCloseNormal, nullslice);
            } else {
                Log("Server unexpectedly closed connection");
                closeC4Socket(WebSocketDomain, kWebSocketCloseAbnormal,
                              "Server unexpectedly closed connection"_sl);
            }
        });
    }


    void LWSWebSocket::closeC4Socket(C4ErrorDomain domain, int code, C4String message) {
        closeC4Socket(c4error_make(domain, code, message));
    }

    void LWSWebSocket::closeC4Socket(C4Error status) {
        if (_c4socket) {
            if (status.code == 0 || (status.code == kWebSocketCloseNormal
                                     && status.domain == WebSocketDomain)) {
                Log("Calling c4socket_closed()");
            } else {
                alloc_slice message(c4error_getMessage(status));
                LogError("Closing with error: %.*s", SPLAT(message));
            }
            c4socket_closed(_c4socket, status);
            setC4Socket(nullptr);
        }
    }

} }


using namespace litecore::net;


#pragma mark - C4 SOCKET FACTORY:


const C4SocketFactory C4LWSWebSocketFactory {
    kC4NoFraming,
    nullptr, // .context (unused)
    &LWSClientWebSocket::sock_open,
    &LWSWebSocket::sock_write,
    &LWSWebSocket::sock_completedReceive,
    nullptr, // .close (will not be called since I do no framing)
    &LWSWebSocket::sock_requestClose,
    &LWSWebSocket::sock_dispose
};


void RegisterC4LWSWebSocketFactory() {
    static std::once_flag once;
    std::call_once(once, [] {
        c4socket_registerFactory(C4LWSWebSocketFactory);
    });
}
