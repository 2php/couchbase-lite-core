//
// XSocket.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include "Address.hh"
#include "HTTPTypes.hh"
#include "function_ref.hh"
#include "fleece/Fleece.hh"
#include <memory>
#include <thread>

namespace litecore {
    class error;
}
namespace litecore { namespace websocket {
    struct CloseStatus;
    class Headers;
} }
namespace sockpp {
    class stream_socket;
    class tls_context;
}

namespace litecore { namespace net {
    class HTTPLogic;

    /** TCP socket class, using the sockpp library. */
    class XSocket {
    public:
        using slice = fleece::slice;
        using string = std::string;

        XSocket(sockpp::tls_context *ctx =nullptr);
        virtual ~XSocket();

        /// Returns the TLS context, if any, used by this socket.
        sockpp::tls_context* TLSContext();

        /// Closes the socket if it's open.
        void close();

        bool connected() const;
        operator bool() const                   {return connected();}

        C4Error error() const                   {return _error;}

        /// Reads up to \ref byteCount bytes to the location \ref dst.
        /// On EOF returns zero. On other error returns -1.
            __attribute__((warn_unused_result))
        ssize_t read(void *dst, size_t byteCount);

        /// Reads exactly \ref byteCount bytes to the location \ref dst.
        /// On premature EOF, throws exception {WebSocket, 400}.
            __attribute__((warn_unused_result))
        ssize_t readExactly(void *dst, size_t byteCount);

        static constexpr size_t kMaxDelimitedReadSize = 50 * 1024;

        /// Reads from the socket until the \ref delimiter byte sequence is found,
        /// and returns the bytes read ending with the delimiter.
        /// If the delimiter is not found, due to EOF of reading more than \ref maxSize bytes,
        /// throws an exception.
        fleece::alloc_slice readToDelimiter(slice delimiter,
                                            bool includeDelimiter =true,
                                            size_t maxSize =kMaxDelimitedReadSize);

        /// Reads an HTTP body, given the headers.
        /// If there's a Content-Length header, reads that many bytes, otherwise reads till EOF.
        bool readHTTPBody(const websocket::Headers &headers, fleece::alloc_slice &body);

        /// Writes to the socket and returns the number of bytes written:
            __attribute__((warn_unused_result))
        ssize_t write(slice);

        /// Writes all the bytes to the socket.
            __attribute__((warn_unused_result))
        ssize_t write_n(slice);

    protected:
        void setError(C4ErrorDomain, int code, slice message);
        static int mbedToNetworkErrCode(int mbedErr);
        void checkStreamError();
            __attribute__((warn_unused_result))
        bool checkSocketFailure();
        ssize_t _read(void *dst, size_t byteCount);
        void pushUnread(slice);

        std::unique_ptr<sockpp::stream_socket> _socket;
        sockpp::tls_context* _tlsContext = nullptr;

    private:
        static constexpr size_t kReadBufferSize = 8192;
        
        std::thread _reader;
        std::thread _writer;

        C4Error _error {};
        fleece::alloc_slice _unread;        // Data read from socket that's been "pushed back"
        size_t _unreadLen {0};              // Length of valid data in _unread
    };



    /** A client socket, that opens a TCP connection. */
    class XClientSocket : public XSocket {
    public:
        XClientSocket(sockpp::tls_context* =nullptr);

        /// Connects to the host, synchronously. On failure throws an exception.
            __attribute__((warn_unused_result))
        bool connect(const repl::Address &addr);
    };

    

    /** A server-side socket, that handles a client connection. */
    class XResponderSocket : public XSocket {
    public:
        XResponderSocket(sockpp::tls_context* =nullptr);

            __attribute__((warn_unused_result))
        bool acceptSocket(sockpp::stream_socket&&, bool useTLS =false);
            __attribute__((warn_unused_result))
        bool acceptSocket(std::unique_ptr<sockpp::stream_socket>, bool useTLS =false);
    };


} }
