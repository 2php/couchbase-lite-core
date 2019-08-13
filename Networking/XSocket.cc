//
// XSocket.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "XSocket.hh"
#include "WebSocketInterface.hh"
#include "Error.hh"
#include "SecureDigest.hh"
#include "SecureRandomize.hh"
#include "fleece/Fleece.hh"
#include <regex>
#include <string>
#include <sstream>

namespace litecore { namespace net {
    using namespace std;
    using namespace fleece;
    using namespace sockpp;
    using namespace litecore::websocket;


    void XSocket::connect() {
        _socket.reset( new tcp_connector({string(slice(_addr.hostname)), _addr.port}) );
        if (!*_socket)
            _throwLastError();
    }


    void XSocket::close() {
        if (_socket) {
            _socket->close();
        }
    }


    void XSocket::sendHTTPRequest(const string &method, function_ref<void(stringstream&)> fn) {
        stringstream rq;
        rq << method << " " << string(slice(_addr.path)) << " HTTP/1.1\r\n"
            "Host: " << string(slice(_addr.hostname)) << "\r\n";
        fn(rq);
        rq << "\r\n";
        write_n(slice(rq.str()));
    }


    XSocket::Response XSocket::readHTTPResponse() {
        Response response = {};

        string responseData = string(readToDelimiter("\r\n\r\n"_sl));
        if (responseData.empty())
            error::_throw(error::WebSocket, 599); // TODO: error code
        regex responseParser(R"(^HTTP/(\d\.\d) (\d+) ([^\r]*)\r\n)");
        smatch m;
        if (!regex_search(responseData, m, responseParser))
            error::_throw(error::Network, kC4NetErrUnknown);
        response.status = stoi(m[2].str());
        response.message = m[3].str();

        regex headersParser(R"(([\w-]+):\s*([^\r]*)\r\n)");
        sregex_iterator begin(m[0].second, responseData.end(), headersParser);
        sregex_iterator end;
        Encoder enc;
        enc.beginDict();
        for (auto i = begin; i != end; ++i) {
            string name = (*i)[1];
            string value = (*i)[2];
            enc.writeKey(name);
            enc.writeString(value);
        }
        enc.endDict();
        response.headers = AllocedDict(enc.finish());
        return response;
    }


    alloc_slice XSocket::readHTTPBody(AllocedDict headers) {
        alloc_slice body;
        int64_t contentLength;
        if (getIntHeader(headers, "Content-Length"_sl, contentLength)) {
            body.resize(contentLength);
            readExactly((void*)body.buf, (size_t)contentLength);
        } else {
            body.resize(1024);
            size_t length = 0;
            while (true) {
                size_t n = read((void*)&body[length], body.size - length);
                if (n == 0)
                    break;
                length += n;
                if (length == body.size)
                    body.resize(2 * body.size);
            }
            body.resize(length);
        }
        return body;
    }


    string XSocket::sendWebSocketRequest(Dict headers, const string &protocol) {
        uint8_t nonceBuf[16];
        slice nonceBytes(nonceBuf, sizeof(nonceBuf));
        SecureRandomize(nonceBytes);
        string nonce = nonceBytes.base64String();

        sendHTTPRequest("GET", [&](stringstream &rq) {
            rq << "Connection: Upgrade\r\n"
                  "Upgrade: websocket\r\n"
                  "Sec-WebSocket-Version: 13\r\n"
                  "Sec-WebSocket-Key: " << nonce << "\r\n";
            if (!protocol.empty())
                rq << "Sec-WebSocket-Protocol: " << protocol << "\r\n";
            // Custom headers:
            for (Dict::iterator i(headers); i; ++i)
                rq << string(i.keyString()) << ": " << string(i.value().toString()) << "\r\n";
        });
        return nonce;
    }


    bool XSocket::checkWebSocketResponse(const Response &rs,
                                         const string &nonce,
                                         const string &requiredProtocol,
                                         CloseStatus &status) {
        if (rs.status != 101) {
            if (rs.status >= 300)
                status = {kWebSocketClose, rs.status, alloc_slice(rs.message)};
            else
                status = {kWebSocketClose, kCodeProtocolError, "Unexpected HTTP response status"_sl};
            return false;
        }
        if (rs.headers["Connection"_sl].asString() != "Upgrade"_sl
                || rs.headers["Upgrade"_sl].asString() != "websocket"_sl) {
            status = {kWebSocketClose, kCodeProtocolError, "Server failed to upgrade connection"_sl};
            return false;
        }
        if (!requiredProtocol.empty()
                && rs.headers["Sec-WebSocket-Protocol"_sl].asString() != slice(requiredProtocol)) {
            status = {kWebSocketClose, 403, "Server did not accept BLIP replication protocol"_sl};
            return false;
        }

        // Check the returned nonce:
        SHA1 digest{slice(nonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")};
        string resultNonce = slice(&digest, sizeof(digest)).base64String();
        if (rs.headers["Sec-WebSocket-Accept"].asString() != slice(resultNonce)) {
            status = {kWebSocketClose, kCodeProtocolError, "Server returned invalid nonce"_sl};
            return false;
        }
        return true;
    }


#pragma mark - LOW-LEVEL WRITING:


    size_t XSocket::write(slice data) {
        ssize_t written = _socket->write(data.buf, data.size);
        if (written < 0) {
            if (_socket->last_error() == EBADF)
                return 0;
            _throwLastError();
        }
        return size_t(written);
    }


    size_t XSocket::write_n(slice data) {
        ssize_t written = _socket->write_n(data.buf, data.size);
        if (written < 0) {
            if (_socket->last_error() == EBADF)
                return 0;
            _throwLastError();
        }
        return size_t(written);
    }


#pragma mark - LOW-LEVEL READING:


    // Primitive unbuffered read call. If an error occurs, throws an exception. Returns 0 on EOF.
    size_t XSocket::_read(void *dst, size_t byteCount) {
        ssize_t n = _socket->read(dst, byteCount);
        if (n < 0) {
            if (_socket->last_error() == EBADF)
                return 0;
            _throwLastError();
        }
        return n;
    }


    size_t XSocket::read(void *dst, size_t byteCount) {
        if (_inputLen > 0) {
            // Use up anything left in the buffer:
            size_t n = min(byteCount, _inputLen);
            memmove(dst, _inputStart, n);
            _inputLen -= n;
            _inputStart += n;
            return n;
        } else {
            return _read(dst, byteCount);
        }
    }

    void XSocket::readExactly(void *dst, size_t byteCount) {
        while (byteCount > 0) {
            auto n = read(dst, byteCount);
            if (n == 0)
                error::_throw(error::WebSocket, 599);  // unexpected EOF // TODO: error code
            byteCount -= n;
            dst = offsetby(dst, n);
        }
    }

    // Read into the internal buffer
    slice XSocket::read(size_t byteCount) {
        if (_inputLen > 0) {
            // Use up anything left in the buffer
            const void *dst = _inputStart;
            return slice(dst, read(_inputStart, byteCount));
        } else {
            size_t n = _read((void*)_input.buf, min(_input.size, byteCount));
            if (n == 0)
                return nullslice;
            return {_input.buf, n};
        }
    }


    slice XSocket::readToDelimiter(slice delim) {
        if (_inputLen > 0 && _inputStart > _input.buf) {
            // Slide unread input down to start of buffer:
            memmove((void*)_input.buf, _inputStart, _inputLen);
            _inputStart = (uint8_t*)_input.buf;
        }

        while (true) {
            // Look for delimiter:
            slice found = slice(_inputStart, _inputLen).find(delim);
            if (found) {
                _inputStart = (uint8_t*)found.end();
                _inputLen -= (_inputStart - (uint8_t*)_input.buf);
                return slice(_input.buf, found.end());
            }

            // Give up if buffer is full:
            if (_inputLen >= _input.size)
                return nullslice; // give up

            // Read more bytes:
            ssize_t n = _read(_inputStart + _inputLen, _input.size - _inputLen);
            if (n == 0)
                return nullslice;
            _inputLen += n;
        }
    }


#pragma mark - UTILITIES:


    void XSocket::_throwLastError() {
        error::_throw(error::POSIX, _socket->last_error());
    }


    bool XSocket::getIntHeader(Dict headers, slice key, int64_t &value) {
        slice v = headers[key].asString();
        if (!v)
            return false;
        try {
            value = stoi(string(v));
            return true;
        } catch (exception &x) {
            return false;
        }
    }

} }
