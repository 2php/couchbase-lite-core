//
// LWSServer.cc
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

#include "LWSServer.hh"
#include "LWSResponder.hh"
#include "LWSContext.hh"
#include "LWSUtil.hh"
#include "Error.hh"


namespace litecore { namespace net {
    using namespace std;

    LWSServer::LWSServer()
    :_mount(new lws_http_mount)
    {
        memset(_mount.get(), 0, sizeof(*_mount));
        _mount->mountpoint = "/";
        _mount->mountpoint_len = 1;
        _mount->protocol = LWSContext::kHTTPServerProtocol;
        _mount->origin_protocol = LWSMPRO_CALLBACK;
    }


    LWSServer::~LWSServer() {
        DebugAssert(!_vhost);
    }


    void LWSServer::start(uint16_t port, const char *hostname) {
        Assert(!_started);

        retain(this);       // balanced by release on LWS_CALLBACK_PROTOCOL_DESTROY
        LWSContext::instance().startServer(this, port, hostname, _mount.get());

        // Block till server starts:
        unique_lock<mutex> lock(_mutex);
        _condition.wait(lock, [&]() {return _started;});
    }


    void LWSServer::stop() {
        if (!_started)
            return;
        
        LWSContext::instance().stop(this);

        // Block till server stops:
        unique_lock<mutex> lock(_mutex);
        _condition.wait(lock, [&]() {return !_started;});
    }


    void LWSServer::createdVHost(lws_vhost *vhost) {
        _vhost = vhost;
        if (!vhost)
            Warn("Unable to create libwebsockets vhost!");
    }


    int LWSServer::dispatch(lws *client, int reason, void *user, void *in, size_t len) {
        switch ((lws_callback_reasons)reason) {
            case LWS_CALLBACK_PROTOCOL_INIT:
                LogDebug("**** LWS_CALLBACK_PROTOCOL_INIT (lws=%p)", client);
                notifyStartStop(true);
                return 0;
            case LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED:
                LogDebug("**** LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED (lws=%p)", client);
                return createResponder(client) ? 0 : -1;
            case LWS_CALLBACK_PROTOCOL_DESTROY:
                LogDebug("**** LWS_CALLBACK_PROTOCOL_DESTROY");
                _vhost = nullptr;
                notifyStartStop(false);
                release(this);
                return 0;
            default:
                if (reason != LWS_CALLBACK_EVENT_WAIT_CANCELLED && (reason < 31 || reason > 36))
                    LogDebug("**** %-s", LWSCallbackName(reason));
                return 0;
        }
    }


    void LWSServer::notifyStartStop(bool started) {
        if (started != _started) {
            unique_lock<mutex> lock(_mutex);
            _started = started;
            _condition.notify_all();
        }
    }


} }
