//
// Created by jun on 2024/7/25.
//
#include <algorithm>
#include <sstream>
#include <regex>
#include <fcntl.h>
#include "photon/extend/xy-http.h"
#include "photon/extend/magic_enum/magic_enum.hpp"
#include "photon/thread/thread.h"
#include "photon/thread/std-compat.h"
#include "photon/common/alog-stdstring.h"
#include "photon/fs/localfs.h"


namespace zyio{

    namespace common{
        char *ZTime::default_date_time_fm = "%Y-%m-%d %H:%M:%S";
        photon::mutex *ZTime::mutex = new photon::mutex();
        ZTime *ZTime::instance = nullptr;
        ZTime *ZTime::initInstance() {
            if (instance == nullptr) {
                mutex->lock();
                if(instance != nullptr){
                    mutex->unlock();
                    return instance;
                }
                // instance is null
                instance = new ZTime();
                auto th = photon_std::thread(&ZTime::start, instance);
                th.detach();
//                photon_std::thread th([&] {
//                    instance->start();
//                });
//                th.detach();
                mutex->unlock();
                return instance;
            }
            return instance;
        }

        ZTime *ZTime::getInstance() {
            return instance;
        }

        ZTime::ZTime() {
            lock = new photon::rwlock();
        }

        ZTime::~ZTime() {
            delete lock;
        }

        void ZTime::start() {
            while (true) {
                lock->lock(photon::WLOCK);

                //init epoch ms now
                auto now = std::chrono::system_clock::now();
                auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                this->epochMsNow = timestamp;

                std::time_t now_time = std::chrono::system_clock::to_time_t(now);
                this->localTimeNow = std::localtime(&now_time);

                char timeString[32];
                auto r = std::strftime(timeString, sizeof(timeString), default_date_time_fm, localTimeNow);
                this->fmNow = estring(timeString, r);

                lock->unlock();
                photon::thread_sleep(1);
            }
        }

        unsigned long ZTime::nowEpochMs() {
            lock->lock(photon::RLOCK);
            DEFER(lock->unlock());
            return epochMsNow;
        }

        estring ZTime::now(const char *fm) {
            lock->lock(photon::RLOCK);
            DEFER(lock->unlock());

            if (fm == nullptr || epochMsNow == 0) {
                return "";
            }

            char timeString[32];
            auto r = std::strftime(timeString, sizeof(timeString), default_date_time_fm, localTimeNow);
            return estring(timeString, r);
        }
    }

    namespace http{



        //    wildcard
//      *          zero or more occurrence of any characters
//      +          1 or more occurrence of any characters
//      ?          any character
//
//	  pattern         url         result
//	  */cmd          test/cmd      true
//	  d/*/cmd        d/test/cmd    true
//	  d/*/cmd        d/e/cmd/g     false
//

        bool urlMatch(const char * url, const char * pattern) {
            for ( ; *url == *pattern || *pattern == '?' ; url++, pattern++)
                if (*url == '\0')
                    return true;

            // a difference was found
            switch(*pattern){
                case '+':
                    if(*url == '\0')
                        break; // end reached before comparing ("foo+" vs "foo")
                    url++;// skip the first character !
                    // treat it as *
                case '*':
                    ++pattern;// pattern point now to the target (next char after *)
                    while(1){
                        // skip char until the target was found or end of url reached
                        for ( ; *url != *pattern && *url != '\0'; url++);

                        if(*url != *pattern) // end reached before the target could be found
                            break;
                        // does the url match the pattern ?
                        if(urlMatch(url,pattern))
                            return true;

                        // else no match, try to find the target furthermore
                        url++;
                    }
                    break;
            }
            return false;
        }


        XyReq::XyReq(char *buf, uint16_t len) :photon::net::http::Request(buf,len) {

        }

        char *XyReq::bodyRead(size_t *len) {
            auto size = body_size();
            *len = size;
            if(size == 0){
                return nullptr;
            }
            char* buf = (char*)malloc(sizeof(char)*size);
            read(buf,size);
            return buf;
        }


        void XyReq::bodyFree(char *buf) {
            if(buf == nullptr){
                return;
            }
            free(buf);
        }


        XyResp::XyResp(char* buf,uint16_t len) : photon::net::http::Response(buf,len) {

        }

        void XyResp::writeBody(ContentType type, char *buf, size_t len) {
            headers.insert("Date",zyio::common::ZTime::getInstance()->now());
            headers.insert("Server","zy-server/1.0");

            auto entry = MimeTypes::types[type];
            if(entry.isTextType){
                auto mimeType = std::string(entry.mimeType);
                mimeType.append(";charset=UTF-8");
                headers.insert("Content-Type",mimeType);
            }else{
                headers.insert("Content-Type",entry.mimeType);
            }
            headers.content_length(len);
            write(buf, len);
        }

        void XyResp::writeBody(std::string contentType, char *buf, size_t len) {
            headers.insert("Date",zyio::common::ZTime::getInstance()->now());
            headers.insert("Server","zy-server/1.0");

            headers.insert("Content-Type",contentType);
            headers.content_length(len);
            write(buf, len);
        }

        void XyResp::writeFile(std::string contentType,char* buf,size_t len,std::string writeFileName) {
            headers.insert("Date",zyio::common::ZTime::getInstance()->now());
            headers.insert("Server","zy-server/1.0");

            headers.insert("Content-Type",contentType);
            std::string fileName = "attachment;filename=\"";
            fileName.append(writeFileName);
            fileName.append("\"");
            headers.insert("Content-Disposition",fileName);
            headers.content_length(len);
            write(buf, len);
        }

       void XyResp::writeFile(ContentType type, const char* fullFilePath,std::string writeFileName) {
           auto entry = MimeTypes::types[type];
           this->writeFile(entry.mimeType,fullFilePath,writeFileName);
        }

        void XyResp::writeFile(std::string contentType, const char* fullFilePath,std::string writeFileName) {
            headers.insert("Date",zyio::common::ZTime::getInstance()->now());
            headers.insert("Server","zy-server/1.0");

            headers.insert("Content-Type",contentType);
            std::string fileName = "attachment;filename=\"";
            fileName.append(writeFileName);
            fileName.append("\"");
            headers.insert("Content-Disposition",fileName);
            auto fs = photon::fs::new_localfs_adaptor(".", photon::fs::ioengine_iouring);
            if (!fs) {
                LOG_ERROR("failed to create fs");
            }
            DEFER(delete fs);
            auto file = fs->open(fullFilePath,O_RDONLY);
            if (!file) {
                LOG_ERROR("failed to open file");
            }
            DEFER(delete file);
            char buf[1024] = {0};
            int totalSize = 0;
            while (true){
                size_t n = file->read(buf,1024);
                if(n <= 0){
                    break;
                }
                write(buf, n);
                totalSize+=n;
            }
            headers.content_length(totalSize);
        }


        UrlPattern::UrlPattern(std::string pattern,photon::net::http::Verb method) : pattern(pattern),method(method){

        }

        std::string UrlPattern::genHttpMapKey(){
            auto verb =  magic_enum::enum_name(method);
            std::stringstream key;
            key<<verb.length();
            key << verb;
            key << pattern;
            return key.str();
        }

        template<typename T>
        UrlPatternMatch<T>::UrlPatternMatch(){
            explicitContainer = new std::unordered_map<std::string,T>();
            vagueContainer = new std::unordered_map<std::string,T>();
        }

        template<typename T>
        UrlPatternMatch<T>::~UrlPatternMatch(){
            delete explicitContainer;
            delete vagueContainer;
        }

        template<typename T>
        bool UrlPatternMatch<T>::isAguePath(std::string path) {
            std::regex pattern(R"([*?+]+)");
            return std::regex_search(path, pattern);
        }

        template<typename T>
        void UrlPatternMatch<T>::addPatternMatch(UrlPattern pattern,T t) {
            auto key = pattern.genHttpMapKey();
            if (isAguePath(pattern.getPattern())) {
                vagueContainer->insert({key,t});
            } else{
                explicitContainer->insert({key,t});
            }
        }

        template<typename T>
        void UrlPatternMatch<T>::addPatternMatch(std::string urlIn,photon::net::http::Verb methodIn,T t) {
            zyio::http::UrlPattern urlPatternIn(urlIn,methodIn);
            addPatternMatch(urlPatternIn,t);
        }

        template<typename T>
        T UrlPatternMatch<T>::doMatch(zyio::http::UrlPattern urlPatternIn) {
            auto key = urlPatternIn.genHttpMapKey();
            auto it = explicitContainer->find(key);
            if(it != explicitContainer->end()){
                return it->second;
            }
            //foreach
            auto methodStr =  magic_enum::enum_name(urlPatternIn.getMethod());
            for(auto it2 : *vagueContainer){
                auto k = it2.first;
                auto idx = k[0]-48;
                auto method2 = k.substr(1,idx);
                if(methodStr == method2){
                    auto pattern = k.substr(idx+1);
                    if(urlMatch(urlPatternIn.getPattern().c_str(),pattern.c_str())){
                        return it2.second;
                    }
                }
            }
            return nullptr;
        }

        template<typename T>
        T UrlPatternMatch<T>::doMatch(std::string urlIn,photon::net::http::Verb methodIn) {
            zyio::http::UrlPattern urlPatternIn(urlIn,methodIn);
            return doMatch(urlPatternIn);
        }




        BizLogicProxy::BizLogicProxy(httpHandler* logic):logic(logic){

        }

        void BizLogicProxy::executeOnce(XyReq &request, XyResp &response) {
            if (hasExecute) {
                return;
            }
            (*logic)(request, response);
            this->hasExecute = true;
        }

        HttpFilter::HttpFilter(int order):order(order) {

        }


        HttpFilterChain::HttpFilterChain(UrlPattern urlPattern) : pattern(urlPattern){
            vector = new std::vector<HttpFilter*>();
        }

        HttpFilterChain::HttpFilterChain(std::string url,photon::net::http::Verb method){
             vector = new std::vector<HttpFilter*>();
             pattern = UrlPattern(url,method);
        }

        HttpFilterChain::~HttpFilterChain(){
            delete vector;
        }

        void HttpFilterChain::addFilter(zyio::http::HttpFilter *filter) {
            vector->push_back(filter);
            std::sort(vector->begin(), vector->end(), [](auto t1, auto t2) -> bool {
                return t1->getOrder() > t2->getOrder();
            });
        }

        bool HttpFilterChain::preHandle(XyReq &req,XyResp &resp) {
            bool result = true;
            for (const auto &item: *vector) {
                if (!item->preHandle(req, resp)) {
                    result = false;
                    break;
                }
            }
            return result;
        }

        void HttpFilterChain::postHandle(BizLogicProxy &logicProxy, XyReq &req,XyResp &resp) {
            for (const auto &item: *vector) {
               item->postHandle(logicProxy, req, resp);
            }
        }

        void HttpFilterChain::afterHandle(XyReq &req,XyResp &resp) {
            for (const auto &item: *vector) {
                item->afterHandle(req, resp);
            }
        }




        XyHttpServer::XyHttpServer() {
            webRouter = new UrlPatternMatch<httpHandler*>();
            webFilterChain = new UrlPatternMatch<HttpFilterChain*>();
            auto time = zyio::common::ZTime::getInstance();
            if(time == nullptr){
                zyio::common::ZTime::initInstance();
            }
        }

        XyHttpServer::~XyHttpServer() noexcept {
            delete webRouter;
            delete webFilterChain;
            status = ServerStatus::stopping;
            for (const auto& it: connections) {
                it->sock->shutdown(ShutdownHow::ReadWrite);
            }
            while (workers != 0) {
                photon::thread_usleep(50 * 1000);
            }

        }



        photon::net::ISocketServer::Handler XyHttpServer::getConnectionHandler() {
            return {this, &XyHttpServer::handleConnection};
        }


        void resp404(XyReq& req, XyResp& resp){
            resp.set_result(404);
            std::string bodyStr = "<p>page not found</p>";
            resp.keep_alive(false);
            resp.headers.insert("Content-Type", "text/html; charset=utf-8");
            resp.write(bodyStr.data(), bodyStr.length());
        }


        int XyHttpServer::handleConnection(photon::net::ISocketStream *sock) {
            workers++;
            DEFER(workers--);
            SockItem sock_item(sock);
            connections.push_back(&sock_item);
            DEFER(connections.erase(&sock_item));

            char req_buf[64*1024];
            char resp_buf[64*1024];
            XyReq req(req_buf, 64*1024-1);
            XyResp resp(resp_buf, 64*1024-1);

            while (status == ServerStatus::running) {
                req.reset(sock, false);

                auto rec_ret = req.receive_header();
                if (rec_ret < 0) {
                    LOG_ERROR_RETURN(0, -1, "read request header failed");
                }
                if (rec_ret == 1) {
                    LOG_DEBUG("exit");
                    return -1;
                }

                LOG_DEBUG("Request Accepted", VALUE(req.verb()), VALUE(req.target()), VALUE(req.headers["Authorization"]),
                          VALUE(req.query()));
                resp.reset(sock, false);
                resp.keep_alive(req.keep_alive());
                resp.headers.insert("Server", "nginx/1.14.1");
                auto verb = req.verb();
                auto url = std::string(req.target());

                auto logic = webRouter->doMatch(url,verb);
                //404
                if (logic == nullptr) {
                    //write 404
                    LOG_DEBUG("404:no handle found");
                    resp404(req,resp);
                    break;
                }
                //wrap logic
                BizLogicProxy logicProxy(logic);
                HttpFilterChain* httpFilterChain = webFilterChain->doMatch(url,verb);
                //no filter chain
                if (httpFilterChain == nullptr) {
                    LOG_DEBUG("no filter found");
                    logicProxy.executeOnce(req, resp);
                    break;
                }
                //pre
                bool res = httpFilterChain->preHandle(req,resp);
                if (!res) {
                    //filter stop handle
                    LOG_DEBUG("filter stop handle");
                    break;
                }
                //post
                httpFilterChain->postHandle(logicProxy, req, resp);
                httpFilterChain->afterHandle(req, resp);

                if (resp.send() < 0) {
                    LOG_ERROR_RETURN(0, -1, "failed to send");
                }

                if (!resp.keep_alive())
                    break;

                if (req.skip_remain() < 0)
                    break;
            }
            return 0;
        }



        void XyHttpServer::bindFilterChain(zyio::http::HttpFilterChain *filterChain) {
            webFilterChain->addPatternMatch(filterChain->getPattern(),filterChain);
        }

        void XyHttpServer::addHandler(std::string pattern,photon::net::http::Verb method,httpHandler* handler){
            webRouter->addPatternMatch(pattern,method,handler);
        }



    }
}
