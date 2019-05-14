//
// LWSContext.cc
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

#include "LWSContext.hh"
#include "Address.hh"
#include "StringUtil.hh"

#ifdef TARGET_OS_OSX
#include <Security/Security.h>
#endif

#include "libwebsockets.h"

namespace litecore { namespace websocket {
    using namespace std;
    using namespace fleece;

    /* "various processes involving network roundtrips in the
     * library are protected from hanging forever by timeouts.  If
     * nonzero, this member lets you set the timeout used in seconds.
     * Otherwise a default timeout is used." */
    static constexpr int kTimeoutSecs = 0;

    // Default idle time after which a PING is sent.
    static constexpr short kDefaultPingIntervalSecs = 5 * 60;


    /** Singleton that manages the libwebsocket context and event thread. */
    void LWSContext::initialize(const struct lws_protocols protocols[]) {
        if (!instance)
            instance = new LWSContext(protocols);
    }

    LWSContext::LWSContext(const struct lws_protocols protocols[]) {
        // Configure libwebsocket logging:
        int flags = LLL_ERR  | LLL_WARN | LLL_NOTICE | LLL_INFO;
#if DEBUG
        flags |= LLL_DEBUG;
#endif
        lws_set_log_level(flags, &logCallback);

        struct lws_context_creation_info info = {};
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.port = CONTEXT_PORT_NO_LISTEN; /* we do not run any server */
        info.protocols = protocols;
        info.vhost_name = "LiteCore";  // if we ran a server, this would have to be the hostname
        info.timeout_secs = kTimeoutSecs;
        info.ws_ping_pong_interval = kDefaultPingIntervalSecs;

#ifdef LWS_WITH_MBEDTLS
        // mbedTLS does not have a list of root CA certs, so get the system list for it:
        alloc_slice systemRootCertsPEM = getSystemRootCertsPEM();
        info.client_ssl_ca_mem = systemRootCertsPEM.buf;
        info.client_ssl_ca_mem_len = (unsigned)systemRootCertsPEM.size;
#endif

        _context = lws_create_context(&info);
        if (!_context)
            return;

        _thread.reset( new thread([&]() {
            while (true) {
                lws_service(_context, 999999);  // TODO: How/when to stop this
            }
        }));
    }


    lws* LWSContext::connect(const litecore::repl::Address &_address,
                             const char *protocol,
                             slice pinnedServerCert,
                             void* opaqueUserData) {
        // Create LWS client and connect:
        string hostname(slice(_address.hostname));
        string path(slice(_address.path));

        struct lws_client_connect_info i = {};
        i.context = LWSContext::instance->context();
        i.port = _address.port;
        i.address = hostname.c_str();
        i.path = path.c_str();
        i.host = i.address;
        i.origin = i.address;
        i.protocol = protocol;
        i.opaque_user_data = opaqueUserData;

        if (_address.isSecure()) {
            i.ssl_connection = LCCSCF_USE_SSL;
            if (pinnedServerCert)
                i.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
        }

        return lws_client_connect_via_info(&i);
    }


    void LWSContext::logCallback(int level, const char *message) {
        slice msg(message);
        if (msg.size > 0 && msg[msg.size-1] == '\n')
            msg.setSize(msg.size-1);
        if (msg.size == 0)
            return;
        C4LogLevel c4level;
        switch(level) {
            case LLL_ERR:    c4level = kC4LogError; break;
            case LLL_WARN:   c4level = kC4LogWarning; break;
            case LLL_NOTICE: c4level = kC4LogInfo; break;
            case LLL_INFO:   c4level = kC4LogInfo; break;
            default:         c4level = kC4LogDebug; break;
        }
        C4LogToAt(kC4WebSocketLog, c4level, "libwebsocket: %.*s", SPLAT(msg));
    }


#ifdef LWS_WITH_MBEDTLS
#ifdef TARGET_OS_OSX
    // Sadly, SecTrustCopyAnchorCertificates() is not available on iOS...
    alloc_slice LWSContext::getSystemRootCertsPEM() {
        CFArrayRef roots;
        OSStatus err = SecTrustCopyAnchorCertificates(&roots);
        if (err)
            return {};
        CFDataRef pemData = nullptr;
        err =  SecItemExport(roots, kSecFormatPEMSequence, kSecItemPemArmour, nullptr, &pemData);
        CFRelease(roots);
        if (err)
            return {};
        alloc_slice pem(CFDataGetBytePtr(pemData), CFDataGetLength(pemData));
        CFRelease(pemData);
        return pem;
    }
#else
    alloc_slice LWSContext::getSystemRootCertsPEM() { return {}; }
#endif
#endif


    LWSContext* LWSContext::instance = nullptr;

} }
