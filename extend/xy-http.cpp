//
// Created by jun on 2024/7/25.
//

#include "photon/extend/xy-http.h"
#include<algorithm>
#include <sstream>
#include "photon/extend/magic_enum/magic_enum.hpp"
#include "photon/thread/thread.h"
#include <photon/common/alog-stdstring.h>
#include <urlmatch.h>
#include <regex>

namespace zyio{
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
        void UrlPatternMatch<T>::addPatternMatch(photon::net::http::Verb methodIn,std::string urlIn,T t) {
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
        T UrlPatternMatch<T>::doMatch(photon::net::http::Verb methodIn, std::string urlIn) {
            zyio::http::UrlPattern urlPatternIn(urlIn,methodIn);
            return doMatch(urlPatternIn);
        }




        BizLogicProxy::BizLogicProxy(httpHandler* logic):logic(logic){

        }

        void BizLogicProxy::executeOnce(photon::net::http::Request &request, photon::net::http::Response &response) {
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

        HttpFilterChain::HttpFilterChain(photon::net::http::Verb method,std::string url){
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

        bool HttpFilterChain::preHandle(photon::net::http::Request &req,
                                        photon::net::http::Response &resp) {
            bool result = true;
            for (const auto &item: *vector) {
                if (!item->preHandle(req, resp)) {
                    result = false;
                    break;
                }
            }
            return result;
        }

        void HttpFilterChain::postHandle(BizLogicProxy &logicProxy, photon::net::http::Request &req,
                                         photon::net::http::Response &resp) {
            for (const auto &item: *vector) {
               item->postHandle(logicProxy, req, resp);
            }
        }

        void HttpFilterChain::afterHandle(photon::net::http::Request &req,
                                          photon::net::http::Response &resp) {
            for (const auto &item: *vector) {
                item->afterHandle(req, resp);
            }
        }




        XyHttpServer::XyHttpServer() {
            webRouter = new UrlPatternMatch<httpHandler*>();
            webFilterChain = new UrlPatternMatch<HttpFilterChain*>();
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


        void resp404(photon::net::http::Request& req, photon::net::http::Response& resp){
            resp.set_result(404);
            std::string bodyStr = "<p>page not found</p>";
            resp.keep_alive(false);
            resp.headers.insert("Content-Type", "text/html; charset=utf-8");
            resp.write(bodyStr.data(), bodyStr.length());
        }

        void test(photon::net::http::Request &req){

        }

        int XyHttpServer::handleConnection(photon::net::ISocketStream *sock) {
            workers++;
            DEFER(workers--);
            SockItem sock_item(sock);
            connections.push_back(&sock_item);
            DEFER(connections.erase(&sock_item));

            char req_buf[64*1024];
            char resp_buf[64*1024];
            photon::net::http::Request req(req_buf, 64*1024-1);
            photon::net::http::Response resp(resp_buf, 64*1024-1);

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

                auto logic = webRouter->doMatch(verb,url);
                //404
                if (logic == nullptr) {
                    //write 404
                    LOG_DEBUG("404:no handle found");
                    resp404(req,resp);
                    break;
                }
                //wrap logic
                BizLogicProxy logicProxy(logic);
                HttpFilterChain* httpFilterChain = webFilterChain->doMatch(verb,url);
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

        void XyHttpServer::addHandler(httpHandler* handler,std::string pattern,photon::net::http::Verb method){
            webRouter->addPatternMatch(method,pattern,handler);
        }



    }
}
