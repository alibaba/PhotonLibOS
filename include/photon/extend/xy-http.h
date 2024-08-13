//
// Created by jun on 2024/7/25.
//

#ifndef PHOTON_EXTEND_XY_HTTP_H
#define PHOTON_EXTEND_XY_HTTP_H

#include <regex>
#include "photon/extend/comm-def.h"
#include "photon/net/http/server.h"
#include "photon/thread/list.h"
#include "photon/thread/thread.h"
#include "MimeTypes.h"

namespace zyio{

    namespace common{
        class ZTime {
        private:

            uint64_t epochMsNow = 0;

            estring fmNow = {};

            std::tm* localTimeNow;

            photon::rwlock *lock;

            static char *default_date_time_fm;
            static photon::mutex *mutex;
            static ZTime *instance;

            ZTime();
            void start();

        public:


            ~ZTime();


            unsigned long nowEpochMs();

            estring now(const char *fm = "%Y-%m-%d %H:%M:%S");


            static ZTime *getInstance();

            static ZTime *initInstance();
        };

    }

    namespace http{


        class XyReq : public photon::net::http::Request{
        private:

        public:
            XyReq() =delete;
            XyReq(char* buf,uint16_t len);

            char* bodyRead(size_t *len);

            void bodyFree(char* buf);
        };


        class XyResp : public photon::net::http::Response {
        private:


        public:

            XyResp() =delete;
            XyResp(char* buf,uint16_t len);

            void writeBody(ContentType type,char* buf,size_t len);

            void writeBody(std::string contentType,char* buf,size_t len);

            void writeFile(ContentType type,const char* fullFilePath,std::string writeFileName);

            void writeFile(std::string contentType,const char* fullFilePath,std::string writeFileName);

            void writeFile(std::string contentType,char* buf,size_t len,std::string writeFileName);

        };



        typedef std::function<void(XyReq&, XyResp&)> httpHandler;


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

            void executeOnce(XyReq &request, XyResp &response);
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
            virtual bool preHandle(XyReq& req, XyResp& resp) = 0;

            virtual void postHandle(BizLogicProxy &bizLogicProxy,XyReq& req, XyResp& resp) = 0;

            virtual void afterHandle(XyReq& req, XyResp& resp) = 0;
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

            bool preHandle(XyReq& req, XyResp& resp);

            void postHandle(BizLogicProxy &logicProxy,XyReq& req, XyResp& resp);

            void afterHandle(XyReq& req, XyResp& resp);

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
