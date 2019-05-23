//
// LWSUtil.cc
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

#include "LWSUtil.hh"
#include "HTTPTypes.hh"

namespace litecore { namespace net {

#if DEBUG
    // Derived from the declaration of `enum lws_callback_reasons` in lws-callbacks.h
    static const char* kCallbackName[102] = {
        /* 0*/ "LWS_CALLBACK_ESTABLISHED",
        /* 1*/ "LWS_CALLBACK_CLIENT_CONNECTION_ERROR",
        /* 2*/ "LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH",
        /* 3*/ "LWS_CALLBACK_CLIENT_ESTABLISHED",
        /* 4*/ "LWS_CALLBACK_CLOSED",
        /* 5*/ "LWS_CALLBACK_CLOSED_HTTP",
        /* 6*/ "LWS_CALLBACK_RECEIVE",
        /* 7*/ "LWS_CALLBACK_RECEIVE_PONG",
        /* 8*/ "LWS_CALLBACK_CLIENT_RECEIVE",
        /* 9*/ "LWS_CALLBACK_CLIENT_RECEIVE_PONG",
        /*10*/ "LWS_CALLBACK_CLIENT_WRITEABLE",
        /*11*/ "LWS_CALLBACK_SERVER_WRITEABLE",
        /*12*/ "LWS_CALLBACK_HTTP",
        /*13*/ "LWS_CALLBACK_HTTP_BODY",
        /*14*/ "LWS_CALLBACK_HTTP_BODY_COMPLETION",
        /*15*/ "LWS_CALLBACK_HTTP_FILE_COMPLETION",
        /*16*/ "LWS_CALLBACK_HTTP_WRITEABLE",
        /*17*/ "LWS_CALLBACK_FILTER_NETWORK_CONNECTION",
        /*18*/ "LWS_CALLBACK_FILTER_HTTP_CONNECTION",
        /*19*/ "LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED",
        /*20*/ "LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION",
        /*21*/ "LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS",
        /*22*/ "LWS_CALLBACK_OPENSSL_LOAD_EXTRA_SERVER_VERIFY_CERTS",
        /*23*/ "LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION",
        /*24*/ "LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER",
        /*25*/ "LWS_CALLBACK_CONFIRM_EXTENSION_OKAY",
        /*26*/ "LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED",
        /*27*/ "LWS_CALLBACK_PROTOCOL_INIT",
        /*28*/ "LWS_CALLBACK_PROTOCOL_DESTROY",
        /*29*/ "LWS_CALLBACK_WSI_CREATE",
        /*30*/ "LWS_CALLBACK_WSI_DESTROY",
        /*31*/ "LWS_CALLBACK_GET_THREAD_ID",
        /*32*/ "LWS_CALLBACK_ADD_POLL_FD",
        /*33*/ "LWS_CALLBACK_DEL_POLL_FD",
        /*34*/ "LWS_CALLBACK_CHANGE_MODE_POLL_FD",
        /*35*/ "LWS_CALLBACK_LOCK_POLL",
        /*36*/ "LWS_CALLBACK_UNLOCK_POLL",
        /*37*/ "LWS_CALLBACK_OPENSSL_CONTEXT_REQUIRES_PRIVATE_KEY",
        /*38*/ "LWS_CALLBACK_WS_PEER_INITIATED_CLOSE",
        /*39*/ "LWS_CALLBACK_WS_EXT_DEFAULTS",
        /*40*/ "LWS_CALLBACK_CGI",
        /*41*/ "LWS_CALLBACK_CGI_TERMINATED",
        /*42*/ "LWS_CALLBACK_CGI_STDIN_DATA",
        /*43*/ "LWS_CALLBACK_CGI_STDIN_COMPLETED",
        /*44*/ "LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP",
        /*45*/ "LWS_CALLBACK_CLOSED_CLIENT_HTTP",
        /*46*/ "LWS_CALLBACK_RECEIVE_CLIENT_HTTP",
        /*47*/ "LWS_CALLBACK_COMPLETED_CLIENT_HTTP",
        /*48*/ "LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ",
        /*49*/ "LWS_CALLBACK_HTTP_BIND_PROTOCOL",
        /*50*/ "LWS_CALLBACK_HTTP_DROP_PROTOCOL",
        /*51*/ "LWS_CALLBACK_CHECK_ACCESS_RIGHTS",
        /*52*/ "LWS_CALLBACK_PROCESS_HTML",
        /*53*/ "LWS_CALLBACK_ADD_HEADERS",
        /*54*/ "LWS_CALLBACK_SESSION_INFO",
        /*55*/ "LWS_CALLBACK_GS_EVENT",
        /*56*/ "LWS_CALLBACK_HTTP_PMO",
        /*57*/ "LWS_CALLBACK_CLIENT_HTTP_WRITEABLE",
        /*58*/ "LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION",
        /*59*/ "LWS_CALLBACK_RAW_RX",
        /*60*/ "LWS_CALLBACK_RAW_CLOSE",
        /*61*/ "LWS_CALLBACK_RAW_WRITEABLE",
        /*62*/ "LWS_CALLBACK_RAW_ADOPT",
        /*63*/ "LWS_CALLBACK_RAW_ADOPT_FILE",
        /*64*/ "LWS_CALLBACK_RAW_RX_FILE",
        /*65*/ "LWS_CALLBACK_RAW_WRITEABLE_FILE",
        /*66*/ "LWS_CALLBACK_RAW_CLOSE_FILE",
        /*67*/ "LWS_CALLBACK_SSL_INFO",
        /*68*/ "(68)",
        /*69*/ "LWS_CALLBACK_CHILD_CLOSING",
        /*70*/ "LWS_CALLBACK_CGI_PROCESS_ATTACH",
        /*71*/ "LWS_CALLBACK_EVENT_WAIT_CANCELLED",
        /*72*/ "LWS_CALLBACK_VHOST_CERT_AGING",
        /*73*/ "LWS_CALLBACK_TIMER",
        /*74*/ "LWS_CALLBACK_VHOST_CERT_UPDATE",
        /*75*/ "LWS_CALLBACK_CLIENT_CLOSED",
        /*76*/ "LWS_CALLBACK_CLIENT_HTTP_DROP_PROTOCOL",
        /*77*/ "LWS_CALLBACK_WS_SERVER_BIND_PROTOCOL",
        /*78*/ "LWS_CALLBACK_WS_SERVER_DROP_PROTOCOL",
        /*79*/ "LWS_CALLBACK_WS_CLIENT_BIND_PROTOCOL",
        /*80*/ "LWS_CALLBACK_WS_CLIENT_DROP_PROTOCOL",
        /*81*/ "LWS_CALLBACK_RAW_SKT_BIND_PROTOCOL",
        /*82*/ "LWS_CALLBACK_RAW_SKT_DROP_PROTOCOL",
        /*83*/ "LWS_CALLBACK_RAW_FILE_BIND_PROTOCOL",
        /*84*/ "LWS_CALLBACK_RAW_FILE_DROP_PROTOCOL",
        /*85*/ "LWS_CALLBACK_CLIENT_HTTP_BIND_PROTOCOL",
        /*86*/ "LWS_CALLBACK_HTTP_CONFIRM_UPGRADE",
        /*87*/ "(87)",
        /*88*/ "(88)",
        /*89*/ "LWS_CALLBACK_RAW_PROXY_CLI_RX",
        /*90*/ "LWS_CALLBACK_RAW_PROXY_SRV_RX",
        /*91*/ "LWS_CALLBACK_RAW_PROXY_CLI_CLOSE",
        /*92*/ "LWS_CALLBACK_RAW_PROXY_SRV_CLOSE",
        /*93*/ "LWS_CALLBACK_RAW_PROXY_CLI_WRITEABLE",
        /*94*/ "LWS_CALLBACK_RAW_PROXY_SRV_WRITEABLE",
        /*95*/ "LWS_CALLBACK_RAW_PROXY_CLI_ADOPT",
        /*96*/ "LWS_CALLBACK_RAW_PROXY_SRV_ADOPT",
        /*97*/ "LWS_CALLBACK_RAW_PROXY_CLI_BIND_PROTOCOL",
        /*98*/ "LWS_CALLBACK_RAW_PROXY_SRV_BIND_PROTOCOL",
        /*99*/ "LWS_CALLBACK_RAW_PROXY_CLI_DROP_PROTOCOL",
        /*100*/ "LWS_CALLBACK_RAW_PROXY_SRV_DROP_PROTOCOL",
        /*101*/ "LWS_CALLBACK_RAW_CONNECTED",
    };

    
    const char* LWSCallbackName(int/*lws_callback_reasons*/ reason) {
        if (reason <= LWS_CALLBACK_RAW_CONNECTED)
            return kCallbackName[reason];
        else
            return "??";
    }
#endif // DEBUG

} }


namespace litecore { namespace REST {

    static const struct {HTTPStatus code; const char* message;} kHTTPStatusMessages[] = {
        {HTTPStatus::OK,                 "OK"},
        {HTTPStatus::Created,            "Created"},
        {HTTPStatus::NoContent,          "No Content"},
        {HTTPStatus::BadRequest,         "Invalid Request"},
        {HTTPStatus::Unauthorized,       "Unauthorized"},
        {HTTPStatus::Forbidden,          "Forbidden"},
        {HTTPStatus::NotFound,           "Not Found"},
        {HTTPStatus::MethodNotAllowed,   "Method Not Allowed"},
        {HTTPStatus::NotAcceptable,      "Not Acceptable"},
        {HTTPStatus::Conflict,           "Conflict"},
        {HTTPStatus::Gone,               "Gone"},
        {HTTPStatus::PreconditionFailed, "Precondition Failed"},
        {HTTPStatus::ServerError,        "Internal Server Error"},
        {HTTPStatus::NotImplemented,     "Not Implemented"},
        {HTTPStatus::GatewayError,       "Bad Gateway"},
        {HTTPStatus::undefined,          nullptr}
    };

    const char* StatusMessage(HTTPStatus code) {
        for (unsigned i = 0; kHTTPStatusMessages[i].message; ++i) {
            if (kHTTPStatusMessages[i].code == code)
                return kHTTPStatusMessages[i].message;
        }
        return nullptr;
    }


    const char* MethodName(Method method) {
        const char* kMethodNames[] = {"GET", "PUT", "DELETE", "POST", "OPTIONS"};
        int shift = -1;
        for (auto m = (unsigned)method; m != 0; m >>= 1)
            ++shift;
        if (shift < 0 || shift >= 5)
            return "??";
        return kMethodNames[shift];
    }

}}
