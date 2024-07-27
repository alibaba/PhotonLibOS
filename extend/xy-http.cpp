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

        std::string genHttpMapKey(std::string url,photon::net::http::Verb method){
            auto verb =  magic_enum::enum_name(method);
            std::stringstream key;
            key<<verb.length();
            key << verb;
            key << url;
            return key.str();
        }

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

        template<typename T>
        Router<T>::Router(std::string pattern,photon::net::http::Verb method,T holder){

        }

        template <typename T>
        T Router<T>::getHolder(){
            return holder;
        }

        template<typename T>
        void WebRouter<T>::addRouter(Router<T> router) {
            auto key = genHttpMapKey(router.getPattern(),router.getMethod());
            if (isAguePath(router.getPattern())) {
                vagueContainer.insert({key,router});
            } else{
                explicitContainer.insert({key,router});
            }
        }

        template<typename T>
        Router<T> WebRouter<T>::doMatch(photon::net::http::Verb method, std::string url) {
            auto key = genHttpMapKey(url,method);
            auto it = explicitContainer.find(url);
            if(it != explicitContainer.end()){
                return it->second;
            }
            //foreach
            auto methodStr =  magic_enum::enum_name(method);
            for(auto it2 : vagueContainer){
                auto k = it2.first;
                auto method2 = k.substr(1,k[0]);
                if(methodStr == method2){
                     auto pattern = k.substr(k[0]+1);
                     if(urlMatch(url,pattern)){
                         return it2.second;
                     }
                }
            }
            return nullptr;
        }

        template<typename T>
        bool WebRouter<T>::isAguePath(std::string path) {
            std::regex pattern(R"([*?+]+)");
            return std::regex_search(path, pattern);
        }


        BizLogicProxy::BizLogicProxy(httpPHandler logic):logic(std::move(logic)){

        }

        void BizLogicProxy::executeOnce(photon::net::http::Request &request, photon::net::http::Response &response) {
            if (hasExecute) {
                return;
            }
            logic(request, response);
            this->hasExecute = true;
        }

        HttpFilter::HttpFilter(int order):order(order) {

        }

        HttpFilterChain::HttpFilterChain(std::string pattern,photon::net::http::Verb method) : pattern(pattern),method(method){
            chain = new std::vector<HttpFilter*>();
        }
        HttpFilterChain::~HttpFilterChain() {
            delete chain;
        }

        void HttpFilterChain::addFilter(zyio::http::HttpFilter *filter) {
            chain->push_back(filter);
            std::sort(chain->begin(), chain->end(), [](auto t1, auto t2) -> bool {
                return t1->getOrder() > t2->getOrder();
            });
        }

        bool HttpFilterChain::preHandle(photon::net::http::Request &req,
                                        photon::net::http::Response &resp) {
            bool result = true;
            for (const auto &item: *chain) {
                if (!item->preHandle(req, resp)) {
                    result = false;
                    break;
                }
            }
            return result;
        }

        void HttpFilterChain::postHandle(BizLogicProxy &logicProxy, photon::net::http::Request &req,
                                         photon::net::http::Response &resp) {
            for (const auto &item: *chain) {
               item->postHandle(logicProxy, req, resp);
            }
        }

        void HttpFilterChain::afterHandle(photon::net::http::Request &req,
                                          photon::net::http::Response &resp) {
            for (const auto &item: *chain) {
                item->afterHandle(req, resp);
            }
        }


        XyHttpServer::~XyHttpServer() noexcept {
            status = ServerStatus::stopping;
            for (const auto& it: connections) {
                it->sock->shutdown(ShutdownHow::ReadWrite);
            }
            while (workers != 0) {
                photon::thread_usleep(50 * 1000);
            }
        }

//        int not_found_handler(photon::net::http::Request& req, photon::net::http::Response& resp) {
//            resp.set_result(404);
//            resp.headers.content_length(0);
//            return 0;
//        }
//
//        int error_handler(photon::net::http::Request& req, photon::net::http::Response& resp) {
//            resp.set_result(500);
//            resp.headers.content_length(0);
//            return 0;
//        }

        photon::net::ISocketServer::Handler XyHttpServer::getConnectionHandler() {
            return {this, &XyHttpServer::handleConnection};
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

                LOG_DEBUG("Request Accepted", VALUE(req.verb()), VALUE(req.target()), VALUE(req.headers["Authorization"]));

                resp.reset(sock, false);
                resp.keep_alive(req.keep_alive());

                auto logic = findHttpPHandler(req);
                //404
                if (logic == nullptr) {
                    //write 404
                    break;
                }
                //wrap logic
                BizLogicProxy logicProxy(logic);
                HttpFilterChain* httpFilterChain = matchFilterChain(req);
                //no filter chain
                if (httpFilterChain == nullptr) {
                    logicProxy.executeOnce(req, resp);
                    break;
                }
                //pre
                bool res = httpFilterChain->preHandle(req,resp);
                if (!res) {
                    //filter stop handle
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
            std::string key = genHttpMapKey(filterChain->getPattern(),filterChain->getMethod());
            chainContainer.insert({key,filterChain});
        }

        void XyHttpServer::addHandler(httpPHandler handler,std::string pattern,photon::net::http::Verb method){
            std::string key = genHttpMapKey(pattern,method);
            handlerContainer.insert({key,handler});
        }

        HttpFilterChain* XyHttpServer::matchFilterChain(photon::net::http::Request &req) {
            auto key = genHttpMapKey(std::string(req.target()) ,req.verb());
            for (auto m: chainContainer) {
                if (url_simplematch(m.first.c_str(), key.c_str())) {
                    auto chain = m.second;
                    return chain;
                }
            }
            return nullptr;
        }

        httpPHandler XyHttpServer::findHttpPHandler(photon::net::http::Request &req) {
            auto key = genHttpMapKey(std::string(req.target()) ,req.verb());
            auto value = handlerContainer.find(key);
            if (value != handlerContainer.end()) {
                return value->second;
            }
            return nullptr;
        }


    }
}
