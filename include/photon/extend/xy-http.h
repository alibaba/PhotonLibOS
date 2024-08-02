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

        typedef std::function<void(photon::net::http::Request&, photon::net::http::Response&)> httpHandler;

        class XyReq{
        private:
            CLASS_FAST_PROPERTY_GETTER(std::string ,pattern,Pattern)
        };

        class UrlPattern {
        private:
            CLASS_FAST_PROPERTY_COMM(std::string ,pattern,Pattern)
            CLASS_FAST_PROPERTY_COMM(photon::net::http::Verb ,method,Method)
        public:
            UrlPattern() = default;
            UrlPattern(std::string pattern,photon::net::http::Verb method);
            std::string genHttpMapKey();
            virtual ~UrlPattern() = default;
        };


        template<typename T>
        class UrlPatternMatch{

        private:
            std::unordered_map<std::string,T>* explicitContainer;
            std::unordered_map<std::string,T>* vagueContainer;

        protected:
            bool isAguePath(std::string path);
        public:
            UrlPatternMatch();
            virtual ~UrlPatternMatch();

            void addPatternMatch(UrlPattern pattern,T t);
            void addPatternMatch(std::string urlIn,photon::net::http::Verb methodIn,T t);
            T doMatch(UrlPattern urlPatternIn);
            T doMatch(std::string urlIn,photon::net::http::Verb methodIn);
        };


        class BizLogicProxy {
        private:
            httpHandler *logic;
            bool hasExecute = false;
        public:
            explicit BizLogicProxy(httpHandler* logic);

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


        class HttpFilterChain  {

        private:
            CLASS_FAST_PROPERTY_GETTER(UrlPattern ,pattern,Pattern)
            CLASS_FAST_PROPERTY_GETTER(std::vector<HttpFilter*>* , vector,Vector)


        public:
            HttpFilterChain(UrlPattern urlPattern);
            HttpFilterChain(std::string url,photon::net::http::Verb method);
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
            UrlPatternMatch<httpHandler*>* webRouter;
            UrlPatternMatch<HttpFilterChain*>* webFilterChain;


            intrusive_list<SockItem> connections = {};
            ServerStatus status = ServerStatus::running;
            uint64_t workers = 0;
        public:
            XyHttpServer();
            virtual ~XyHttpServer();

            photon::net::ISocketServer::Handler getConnectionHandler();
            int handleConnection(photon::net::ISocketStream* stream);

            void bindFilterChain(HttpFilterChain* filterChain);
            void addHandler(std::string pattern,photon::net::http::Verb method,httpHandler* handler);




        };

    };

}

#endif //PHOTON_EXTEND_XY_HTTP_H
