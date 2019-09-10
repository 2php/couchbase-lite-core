//
// BuiltInWebSocket.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "WebSocketImpl.hh"
#include "TCPSocket.hh"
#include "HTTPLogic.hh"
#include "c4.hh"
#include <atomic>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

extern "C" {
    /** Call this to use BuiltInWebSocket as the WebSocket implementation. */
    void C4RegisterBuiltInWebSocket();
}

namespace sockpp {
    class tls_context;
}

namespace litecore { namespace websocket {

    /** WebSocket implementation using TCPSocket. */
    class BuiltInWebSocket : public WebSocketImpl, public net::HTTPLogic::CookieProvider {
    public:
        static void registerWithReplicator();

        BuiltInWebSocket(const URL &url,
                         const fleece::AllocedDict &options,
                         C4Database *database);

        BuiltInWebSocket(const URL &url,
                         std::unique_ptr<net::ResponderSocket>);

        virtual void connect() override;

    protected:
        ~BuiltInWebSocket();

        // Implementations of WebSocketImpl abstract methods:
        virtual void closeSocket() override;
        virtual void sendBytes(fleece::alloc_slice) override;
        virtual void receiveComplete(size_t byteCount) override;
        virtual void requestClose(int status, fleece::slice message) override;

        // CookieProvider API:
        virtual fleece::alloc_slice cookiesForRequest(const net::Address&) override;
        virtual void setCookie(const net::Address&, fleece::slice cookieHeader) override;

    private:
        void run();
        void setThreadName();
        bool configureProxy(net::HTTPLogic&, fleece::Dict proxyOpt);
        std::unique_ptr<net::ClientSocket> _connectLoop()MUST_USE_RESULT;
        void ioLoop();
        bool readFromSocket();
        bool writeToSocket();
        void closeWithException(const std::exception&, const char *where);
        void closeWithError(C4Error);

        // Max number of bytes read that haven't been processed by the client yet.
        // Beyond this point, I will stop reading from the socket, sending
        // backpressure to the peer.
        static constexpr size_t kReadCapacity = 64 * 1024;

        // Size of the buffer allocated for reading from the socket.
        static constexpr size_t kReadBufferSize = 32 * 1024;

        c4::ref<C4Database> _database;                     // The database (used for cookies)
        std::unique_ptr<net::TCPSocket> _socket;            // The TCP socket
        std::unique_ptr<sockpp::tls_context> _tlsContext;   // TLS settings
        std::thread _ioThread;                              // Thread that reads/writes socket
        std::atomic<bool> _waitingForIO {false};            // Blocked in waitForIO()?

        std::vector<fleece::slice> _outbox;                 // Byte ranges to be sent by writer
        std::vector<fleece::alloc_slice> _outboxAlloced;    // Same, but retains the heap data
        std::mutex _outboxMutex;                            // Locking for outbox

        std::atomic<size_t> _curReadCapacity {kReadCapacity}; // # bytes I can read from socket
        fleece::alloc_slice _readBuffer;
    };


} }
