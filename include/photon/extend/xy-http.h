//
// Created by jun on 2024/7/25.
//

#ifndef PHOTON_EXTEND_XY_HTTP_H
#define PHOTON_EXTEND_XY_HTTP_H

#include "photon/extend/comm-def.h"
#include "photon/net/http/server.h"

namespace zyio{
    namespace http{

        typedef std::function<void(photon::net::http::Request&, photon::net::http::Response&)> bizLogic;

        class BizLogicProxy {
        private:
            bizLogic logic;
            bool hasExecute = false;
        public:
            explicit BizLogicProxy(bizLogic logic);

            void executeOnce(photon::net::http::Request &request, photon::net::http::Response &response);
        };

        class HttpFilter{
        private:
            CLASS_FAST_PROPERTY_GETTER(unsigned int , order, Order)
            HttpFilter() = delete;
        public:
            HttpFilter(int order);

            /**
             * if return true,the filter keep run
             * if return false,the filter will stop,the biz handle logic will not be execute
             * @param req
             * @param resp
             * @return
             */
            virtual bool preHandle(photon::net::http::Request& req, photon::net::http::Response& resp) = 0;

            virtual void postHandle(BizLogicProxy &bizLogicProxy,photon::net::http::Request& req, photon::net::http::Response& resp) = 0;

            virtual void afterHandle(photon::net::http::Request& req, photon::net::http::Response& resp) = 0;
        };

        class HttpFilterChain : public HttpFilter{
        private:
            CLASS_FAST_PROPERTY_GETTER(std::string_view,pattern,Pattern)
            CLASS_FAST_PROPERTY_GETTER(photon::net::http::Verb,method,Method)
            std::vector<HttpFilter*>* chain;
        public:
            HttpFilterChain() = delete;
            HttpFilterChain(std::string_view pattern,photon::net::http::Verb method);
            ~HttpFilterChain();
            void addFilter(HttpFilter* filter);

        };

        class HttpServerEnhance{
        private:
            std::unordered_map<std::string,HttpFilterChain*> chainContainer = {};
        public:
            HttpServerEnhance() = delete;
            ~HttpServerEnhance() = default;

            void bindFilterChain(HttpFilterChain* filterChain);

        protected:
            void findMatchChain(std::vector<HttpFilterChain*> &vector,photon::net::http::Request& req);

            bool preHandle(std::vector<HttpFilterChain*> &vector,photon::net::http::Request& req, photon::net::http::Response& resp);

            void postHandle(std::vector<HttpFilterChain*> &vector,photon::net::http::Request& req, photon::net::http::Response& resp);

            void afterHandle(std::vector<HttpFilterChain*> &vector,photon::net::http::Request& req, photon::net::http::Response& resp);

        };

    };

}

#endif //PHOTON_EXTEND_XY_HTTP_H
