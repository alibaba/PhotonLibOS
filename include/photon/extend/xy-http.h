//
// Created by jun on 2024/7/25.
//

#ifndef PHOTON_EXTEND_XY_HTTP_H
#define PHOTON_EXTEND_XY_HTTP_H

#include <regex>
#include "photon/extend/comm-def.h"
#include "photon/net/http/server.h"
#include "photon/thread/list.h"

namespace zyio{
    namespace http{

        typedef std::function<void(photon::net::http::Request&, photon::net::http::Response&)> httpPHandler;

        template<typename T>
        class Router{
        private:
            CLASS_FAST_PROPERTY_GETTER(std::string ,pattern,Pattern)
            CLASS_FAST_PROPERTY_GETTER(photon::net::http::Verb ,method,Method)
            T holder;
        public:
            Router()=delete;
            Router(std::string pattern,photon::net::http::Verb method,T holder);
            T getHolder();
        };

        template<class T>
        class WebRouter{
        private:
            std::unordered_map<std::string,Router<T>> explicitContainer ={};
            std::unordered_map<std::string,Router<T>> vagueContainer ={};


        public:
            void addRouter(Router<T> router);
            Router<T> doMatch(photon::net::http::Verb method,std::string url);

            bool isAguePath(std::string path);
        };



        class BizLogicProxy {
        private:
            httpPHandler logic;
            bool hasExecute = false;
        public:
            explicit BizLogicProxy(httpPHandler logic);

            void executeOnce(photon::net::http::Request &request, photon::net::http::Response &response);
        };

        class HttpFilter{
        private:
            CLASS_FAST_PROPERTY_GETTER(unsigned int , order, Order)
            HttpFilter() = delete;
        public:
            HttpFilter(int order);

            virtual ~HttpFilter() = default;

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

        class HttpFilterChain {
        private:
            CLASS_FAST_PROPERTY_GETTER(std::string,pattern,Pattern)
            CLASS_FAST_PROPERTY_GETTER(photon::net::http::Verb,method,Method)
            std::vector<HttpFilter*>* chain;
        public:
            HttpFilterChain() = delete;
            HttpFilterChain(std::string pattern,photon::net::http::Verb method);
            virtual ~HttpFilterChain();
            void addFilter(HttpFilter* filter);

            bool preHandle(photon::net::http::Request& req, photon::net::http::Response& resp);

            void postHandle(BizLogicProxy &logicProxy,photon::net::http::Request& req, photon::net::http::Response& resp);

            void afterHandle(photon::net::http::Request& req, photon::net::http::Response& resp);

        };

        enum class ServerStatus {
            running = 1,
            stopping = 2,
        };

        struct SockItem: public intrusive_list_node<SockItem> {
            SockItem(photon::net::ISocketStream* sock): sock(sock) {}
            photon::net::ISocketStream* sock = nullptr;
        };

        class XyHttpServer : public Object {
        private:
            WebRouter<HttpFilterChain*> chainContainer = {};
            WebRouter<httpPHandler> handlerContainer = {};

            intrusive_list<SockItem> connections = {};
            ServerStatus status = ServerStatus::running;
            uint64_t workers = 0;
        public:
            XyHttpServer() = default;
            virtual ~XyHttpServer();

            photon::net::ISocketServer::Handler getConnectionHandler();
            int handleConnection(photon::net::ISocketStream* stream);


            void addHandler(httpPHandler handler,std::string pattern,photon::net::http::Verb method);
            void bindFilterChain(HttpFilterChain* filterChain);





        protected:
            HttpFilterChain* matchFilterChain(photon::net::http::Request& req);
            httpPHandler findHttpPHandler(photon::net::http::Request& req);

        };

    };

}

#endif //PHOTON_EXTEND_XY_HTTP_H
