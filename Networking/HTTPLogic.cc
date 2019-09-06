//
// HTTPLogic.cc
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

#include "HTTPLogic.hh"
#include "TCPSocket.hh"
#include "WebSocketInterface.hh"
#include "c4Replicator.h"
#include "Error.hh"
#include "SecureRandomize.hh"
#include "SecureDigest.hh"
#include "StringUtil.hh"
#include <regex>
#include <sstream>

namespace litecore { namespace net {
    using namespace std;
    using namespace fleece;
    using namespace websocket;


    static constexpr unsigned kMaxRedirects = 10;


    nonstd::optional<ProxySpec> HTTPLogic::sDefaultProxy;


    HTTPLogic::HTTPLogic(const Address &address,
                         bool handleRedirects)
    :_address(address)
    ,_handleRedirects(handleRedirects)
    ,_isWebSocket(address.scheme == "ws"_sl || address.scheme == "wss"_sl)
    ,_proxy(sDefaultProxy)
    { }


    HTTPLogic::HTTPLogic(const Address &address,
                         const websocket::Headers &requestHeaders,
                         bool handleRedirects)
    :HTTPLogic(address, handleRedirects)
    {
        _requestHeaders = requestHeaders;
    }


    HTTPLogic::~HTTPLogic()
    { }


    void HTTPLogic::setHeaders(const websocket::Headers &requestHeaders) {
        Assert(_requestHeaders.empty());
        _requestHeaders = requestHeaders;
    }


    const Address& HTTPLogic::directAddress() {
        if (!_proxy)
            return _address;
        else
            return _proxy->address;
    }


    bool HTTPLogic::connectingToProxy() {
        return _proxy && _proxy->type == ProxyType::CONNECT && _lastDisposition != kContinue;
    }


    static void addHeader(stringstream &rq, const char *key, slice value) {
        if (value)
            rq << key << ": " << string(value) << "\r\n";
    }


    string HTTPLogic::requestToSend() {
        if (_lastDisposition == kAuthenticate) {
            if (_httpStatus == HTTPStatus::ProxyAuthRequired)
                Assert(_proxy && _proxy->authHeader);
            else
                Assert(_authHeader);
        }

        stringstream rq;
        if (connectingToProxy()) {
            // CONNECT proxy: https://tools.ietf.org/html/rfc7231#section-4.3.6
            rq << "CONNECT " << string(slice(_address.hostname)) << ":" << _address.port;
        } else {
            rq << MethodName(_method) << " ";
            if (_proxy && _proxy->type == ProxyType::HTTP)
                rq << string(_address.url());
            else
                rq << string(slice(_address.path));
        }
        rq << " HTTP/1.1\r\n"
              "Host: " << string(slice(_address.hostname)) << ':' << _address.port << "\r\n";
        addHeader(rq, "User-Agent", _userAgent);
        if (_proxy)
            addHeader(rq, "Proxy-Authorization", _proxy->authHeader);
        if (!connectingToProxy()) {
            if (_authHeader && _authChallenged)     // don't send auth until challenged
                addHeader(rq, "Authorization", _authHeader);
            if (_contentLength >= 0)
                rq << "Content-Length: " << _contentLength << "\r\n";
            _requestHeaders.forEach([&](slice name, slice value) {
                rq << string(name) << ": " << string(value) << "\r\n";
            });

            if (_isWebSocket) {
                // WebSocket handshake headers:
                uint8_t nonceBuf[16];
                slice nonceBytes(nonceBuf, sizeof(nonceBuf));
                SecureRandomize(nonceBytes);
                _webSocketNonce = nonceBytes.base64String();
                rq << "Connection: Upgrade\r\n"
                      "Upgrade: websocket\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "Sec-WebSocket-Key: " << _webSocketNonce << "\r\n";
                addHeader(rq, "Sec-WebSocket-Protocol", _webSocketProtocol);
            }
        }

        rq << "\r\n";
        return rq.str();
    }


    alloc_slice HTTPLogic::basicAuth(slice username, slice password) {
        string credential = slice(string(username) + ':' + string(password)).base64String();
        return alloc_slice("Basic " + credential);
    }


#pragma mark - RESPONSE HANDLING:


    HTTPLogic::Disposition HTTPLogic::receivedResponse(slice responseData) {
        _httpStatus = HTTPStatus::undefined;
        _statusMessage = nullslice;
        _responseHeaders.clear();
        _error = {};
        _authChallenge.reset();

        if (parseStatusLine(responseData) && parseHeaders(responseData, _responseHeaders))
            _lastDisposition = handleResponse();
        else
            _lastDisposition = failure(WebSocketDomain, 400, "Received invalid HTTP"_sl);
        return _lastDisposition;
    }


    HTTPLogic::Disposition HTTPLogic::handleResponse() {
        switch (_httpStatus) {
            case HTTPStatus::MovedPermanently:
            case HTTPStatus::Found:
            case HTTPStatus::TemporaryRedirect:
            case HTTPStatus::UseProxy:
                return handleRedirect();
            case HTTPStatus::Unauthorized:
                if (_authChallenged)
                    _authHeader = nullslice;
                else
                    _authChallenged = true;
                return handleAuthChallenge("Www-Authenticate"_sl, false);
            case HTTPStatus::ProxyAuthRequired:
                if (_proxy)
                    _proxy->authHeader = nullslice;
                return handleAuthChallenge("Proxy-Authenticate"_sl, true);
            case HTTPStatus::Upgraded:
                return handleUpgrade();
            default:
                if (!IsSuccess(_httpStatus))
                    return failure();
                else if (connectingToProxy())
                    return kContinue;
                else if (_isWebSocket)
                    return failure(WebSocketDomain, kCodeProtocolError,
                                   "Server failed to upgrade connection"_sl);
                else
                    return kSuccess;
        }
    }


    bool HTTPLogic::parseStatusLine(slice &responseData) {
        slice version = responseData.readToDelimiter(" "_sl);
        uint64_t status = responseData.readDecimal();
        if (!version.hasPrefix("HTTP/"_sl) || status == 0 || status > INT_MAX)
            return false;
        _httpStatus = HTTPStatus(status);
        if (responseData.size == 0 || (responseData[0] != ' ' && responseData[0] != '\r'))
            return false;
        while (responseData.hasPrefix(' '))
            responseData.moveStart(1);
        slice message = responseData.readToDelimiter("\r\n"_sl);
        if (!message)
            return false;
        _statusMessage = alloc_slice(message);
        return true;
    }


    // Reads HTTP headers out of `responseData`. Assumes data ends with CRLFCRLF.
    bool HTTPLogic::parseHeaders(slice &responseData, Headers &headers) {
        while (true) {
            slice line = responseData.readToDelimiter("\r\n"_sl);
            if (!line)
                return false;
            if (line.size == 0)
                break;  // empty line
            const uint8_t *colon = line.findByte(':');
            if (!colon)
                return false;
            slice name(line.buf, colon);
            line.setStart(colon+1);
            const uint8_t *nonSpace = line.findByteNotIn(" "_sl);
            if (!nonSpace)
                return false;
            slice value(nonSpace, line.end());
            headers.add(name, value);
        }
        return true;
    }


    HTTPLogic::Disposition HTTPLogic::handleRedirect() {
        if (!_handleRedirects)
            return failure();
        if (++_redirectCount > kMaxRedirects)
            return failure(NetworkDomain, kC4NetErrTooManyRedirects);

        C4Address newAddr;
        slice location = _responseHeaders["Location"_sl];
        if (location.hasPrefix('/')) {
            newAddr = _address;
            newAddr.path = location;
        } else {
            if (!c4address_fromURL(location, &newAddr, nullptr)
                    || (newAddr.scheme != "http"_sl && newAddr.scheme != "https"_sl))
                return failure(NetworkDomain, kC4NetErrInvalidRedirect);
        }

        if (_httpStatus == HTTPStatus::UseProxy) {
            if (_proxy)
                return failure();
            _proxy = ProxySpec(ProxyType::HTTP, newAddr);
        } else {
            if (newAddr.hostname != _address.hostname)
                _authHeader = nullslice;
            _address = Address(newAddr);
        }
        return kRetry;
    }


    HTTPLogic::Disposition HTTPLogic::handleAuthChallenge(slice headerName, bool forProxy) {
        string authHeader(_responseHeaders[headerName]);
        // Parse the Authenticate header:
        regex authEx(R"((\w+)\s+(\w+)=((\w+)|"([^"]+)))");     // e.g. Basic realm="Foobar"
        smatch m;
        if (!regex_search(authHeader, m, authEx))
            return failure(WebSocketDomain, 400);
        AuthChallenge challenge(forProxy ? _proxy->address : _address, forProxy);
        challenge.type = m[1].str();
        challenge.key = m[2].str();
        challenge.value = m[4].str();
        if (challenge.value.empty())
            challenge.value = m[5].str();
        _authChallenge = challenge;
        if (!forProxy)
            _authChallenged = true;
        return kAuthenticate;
    }


    HTTPLogic::Disposition HTTPLogic::handleUpgrade() {
        if (!_isWebSocket)
            return failure(WebSocketDomain, kCodeProtocolError);

        if (_responseHeaders["Connection"_sl] != "Upgrade"_sl
                || _responseHeaders["Upgrade"_sl] != "websocket"_sl) {
            return failure(WebSocketDomain, kCodeProtocolError,
                           "Server failed to upgrade connection"_sl);
        }

        if (_webSocketProtocol && _responseHeaders["Sec-Websocket-Protocol"_sl] != _webSocketProtocol) {
            return failure(WebSocketDomain, 403, "Server did not accept protocol"_sl);
        }

        // Check the returned nonce:
        SHA1 digest{slice(_webSocketNonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")};
        string resultNonce = slice(&digest, sizeof(digest)).base64String();
        if (_responseHeaders["Sec-Websocket-Accept"_sl] != slice(resultNonce))
            return failure(WebSocketDomain, kCodeProtocolError,
                           "Server returned invalid nonce"_sl);

        return kSuccess;
    }


    HTTPLogic::Disposition HTTPLogic::failure(C4ErrorDomain domain, int code, slice message) {
        Assert(code != 0);
        _error = c4error_make(domain, code, message);
        return kFailure;
    }


    HTTPLogic::Disposition HTTPLogic::failure(ClientSocket &socket) {
        _error = socket.error();
        Assert(_error.code != 0);
        return kFailure;
    }


    HTTPLogic::Disposition HTTPLogic::failure() {
        return failure(WebSocketDomain, int(_httpStatus), _statusMessage);
    }


    HTTPLogic::Disposition HTTPLogic::sendNextRequest(ClientSocket &socket, slice body) {
        bool connected;
        if (_lastDisposition == kContinue) {
            Assert(socket.connected());
            connected = !_address.isSecure() || socket.wrapTLS(_address.hostname);
        } else {
            Assert(!socket.connected());
            connected = socket.connect(directAddress());
        }
        if (!connected)
            return failure(socket);

        C4LogToAt(kC4WebSocketLog, kC4LogVerbose, "Sending request to %s:\n%s",
                  (_lastDisposition == kContinue ? "proxy tunnel"
                                                 : string(directAddress().url()).c_str()),
                  formatHTTP(slice(requestToSend())).c_str());
        if (socket.write_n(requestToSend()) < 0 || socket.write_n(body) < 0)
            return failure(socket);
        alloc_slice response = socket.readToDelimiter("\r\n\r\n"_sl);
        if (!response)
            return failure(socket);
        C4LogToAt(kC4WebSocketLog, kC4LogVerbose, "Got response:\n%s", formatHTTP(response).c_str());
        return receivedResponse(response);
    }


    string HTTPLogic::formatHTTP(slice http) {
        stringstream s;
        bool first = true;
        while (true) {
            slice line = http.readToDelimiter("\r\n"_sl);
            if (line.size == 0)
                break;
            if (!first)
                s << '\n';
            first = false;
            s << '\t';
            s.write((const char*)line.buf, line.size);
        }
        return s.str();
    }


} }