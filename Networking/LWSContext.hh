//
// LWSContext.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "Address.hh"
#include "Channel.hh"
#include "RefCounted.hh"
#include "fleece/slice.hh"
#include <functional>
#include <memory>
#include <string>
#include <thread>

// libwebsocket opaque structs:
struct lws;
struct lws_context;
struct lws_context_creation_info;
struct lws_http_mount;
struct lws_vhost;

namespace litecore { namespace crypto {
    class Cert;
    class Identity;
} }

namespace litecore { namespace net {

    class LWSProtocol;
    class LWSServer;


    /** Singleton that manages the libwebsocket context and event thread. */
    class LWSContext {
    public:
        // Single instance
        static LWSContext& instance();

        // Protocol names for connectClient and startServer:
        static constexpr const char* kBLIPClientProtocol = "BLIP_3+CBMobile_2";
        static constexpr const char* kHTTPClientProtocol = "HTTPClient";
        static constexpr const char* kHTTPServerProtocol = kBLIPClientProtocol;

        void connectClient(LWSProtocol *protocolInstance NONNULL,
                           const char *protocolName NONNULL,
                           const repl::Address &address,
                           fleece::slice pinnedServerCert,  // must remain valid till connected
                           const char *method = nullptr);

        void startServer(LWSServer *server NONNULL,
                         uint16_t port,
                         const char *hostname,
                         const lws_http_mount *mounts NONNULL,
                         crypto::Identity *tlsIdentity);

        void stop(LWSServer* NONNULL);

        lws_context* context() const                {return _context;}

        void dequeue(); // internal use only

    protected:
        void enqueue(std::function<void()> fn);
        const char *className() const noexcept      {return "LWSContext";}  // for logging

    private:
        LWSContext();
        static void initLogging();
        static void logCallback(int level, const char *message);
        std::string getSystemRootCertsPEM();
        void startEventLoop();

        void _connectClient(fleece::Retained<LWSProtocol>,
                            const std::string &protocolName,
                            repl::Address address,
                            fleece::slice pinnedServerCert,
                            const std::string &method);
        void _startServer(fleece::Retained<LWSServer>,
                          uint16_t port,
                          const std::string &hostname,
                          const lws_http_mount *mounts,
                          Retained<crypto::Identity> tlsIdentity);
        void _stop(fleece::Retained<LWSServer>);


        std::unique_ptr<lws_context_creation_info>  _info;
        lws_context*                                _context {nullptr};
        std::unique_ptr<std::thread>                _thread;
        actor::Channel<std::function<void()>>       _enqueued;

        std::string _systemRootCertsPEM;
    };

} }
