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
        operator bool() const   {return connected();}

        /// Reads up to \ref byteCount bytes to the location \ref dst.
        /// On EOF returns zero. On other error throws an exception.
        size_t read(void *dst, size_t byteCount);

        /// Reads exactly \ref byteCount bytes to the location \ref dst.
        /// On EOF returns zero. On other error throws an exception.
        void readExactly(void *dst, size_t byteCount);

        /// Reads into the internal buffer and returns a pointer to the read data
        /// On EOF returns \ref nullslice. On other error throws an exception.
        slice read(size_t byteCount =kReadBufferSize);

        /// Reads into the internal buffer until the \ref delimiter byte sequence is found,
        /// and returns the bytes read ending with the delimiter.
        /// If the buffer fills up before the delimiter is found, or EOF is reached,
        /// returns nullslice.
        /// On error throws an exception.
        /// This method is likely to read bytes past the delimiter! The extra bytes will be
        /// returned by subsequent reads.
        slice readToDelimiter(slice delimiter, bool includeDelimiter =false);

        /// Reads an HTTP body given the headers.
        /// If there's a Content-Length header, reads that many bytes.
        /// Otherwise reads till EOF.
        fleece::alloc_slice readHTTPBody(const websocket::Headers &headers);

        /// Writes to the socket and returns the number of bytes written:
            __attribute__((warn_unused_result))
        size_t write(slice);

        /// Writes all the bytes to the socket.
        size_t write_n(slice);

        // Utility function that maps an exception to a LiteCore error.
        static litecore::error convertException(const std::exception&);

    protected:
        static int mbedToNetworkErrCode(int mbedErr);
        [[noreturn]] void _throwLastError();
        [[noreturn]] void _throwBadHTTP();
        void checkSocketFailure();
        size_t _read(void *dst, size_t byteCount);

        std::unique_ptr<sockpp::stream_socket> _socket;
        sockpp::tls_context* _tlsContext = nullptr;

    private:
        static constexpr size_t kReadBufferSize = 8192;
        
        std::thread _reader;
        std::thread _writer;

        uint8_t _readBuffer[kReadBufferSize];
        slice const _input = {_readBuffer, sizeof(_readBuffer)};
        uint8_t *_inputStart = (uint8_t*)_input.buf;
        size_t _inputLen = 0;
    };



    /** A client socket, that opens a TCP connection. */
    class XClientSocket : public XSocket {
    public:
        XClientSocket(sockpp::tls_context* =nullptr);

        /// Connects to the host, synchronously. On failure throws an exception.
        void connect(const repl::Address &addr);
    };

    

    /** A server-side socket, that handles a client connection. */
    class XResponderSocket : public XSocket {
    public:
        XResponderSocket(sockpp::tls_context* =nullptr);

        void acceptSocket(sockpp::stream_socket&&, bool useTLS =false);
        void acceptSocket(std::unique_ptr<sockpp::stream_socket>, bool useTLS =false);
    };


} }
