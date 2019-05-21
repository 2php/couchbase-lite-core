//
// LWSServer.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include <mutex>
#include <memory>

struct lws;
struct lws_http_mount;
struct lws_vhost;

namespace litecore { namespace REST {
    class LWSResponder;
} }


namespace litecore { namespace websocket {

    class LWSServer : public fleece::RefCounted {
    public:
        LWSServer(uint16_t port, const char *hostname);
        
        virtual int dispatch(lws*, int callback_reason, void *user, void *in, size_t len);

        virtual void dispatchResponder(REST::LWSResponder*) =0;

        virtual const char *className() const noexcept      {return "LWSServer";}

    protected:
        virtual ~LWSServer();
        virtual bool createResponder(lws *client);
        
    private:
        void createdVHost(lws_vhost*);

        std::mutex _mutex;
        std::unique_ptr<lws_http_mount> _mount;
        lws_vhost* _vhost {nullptr};

        friend class LWSContext;
    };

} }
