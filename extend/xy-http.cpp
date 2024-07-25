//
// Created by jun on 2024/7/25.
//

#include "photon/extend/xy-http.h"
#include<algorithm>
#include <sstream>
#include "photon/extend/magic_enum/magic_enum.hpp"
#include "photon/extend/urlmatch/urlmatch.h"

namespace zyio{
    namespace http{

        BizLogicProxy::BizLogicProxy(bizLogic logic):logic(std::move(logic)){

        }

        void BizLogicProxy::executeOnce(photon::net::http::Request &request, photon::net::http::Response &response) {
            if (hasExecute) {
                return;
            }
            logic(request, response);
            hasExecute = true;
        }

        HttpFilter::HttpFilter(int order):order(order) {

        }

        HttpFilterChain::HttpFilterChain(std::string_view pattern,photon::net::http::Verb method) :HttpFilter(1), pattern(pattern),method(method){
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


        void HttpServerEnhance::bindFilterChain(zyio::http::HttpFilterChain *filterChain) {
            auto method =  magic_enum::enum_name(filterChain->getMethod());
            std::stringstream key;
            key << filterChain->getPattern();
            key << "@";
            key << method;
            chainContainer.insert({key.str(),filterChain});
        }

        void HttpServerEnhance::findMatchChain(std::vector<HttpFilterChain *> &vector,
                                               photon::net::http::Request &req) {
            auto method =  magic_enum::enum_name(req.verb());
            std::stringstream key;
            key << req.target();
            key << "@";
            key << method;

            for (auto m: chainContainer) {
                if (url_simplematch(m.first.c_str(), key.str().c_str())) {
                    auto chain = m.second;
                    vector.push_back(chain);
                }
            }
        }

        bool HttpServerEnhance::preHandle(std::vector<HttpFilterChain *> &vector, photon::net::http::Request &req,
                                          photon::net::http::Response &resp) {
            for (auto chain: vector) {
                chain->preHandle();
            }
        }

        void HttpServerEnhance::postHandle(std::vector<HttpFilterChain *> &vector, photon::net::http::Request &req,
                                           photon::net::http::Response &resp) {

        }

        void HttpServerEnhance::afterHandle(std::vector<HttpFilterChain *> &vector, photon::net::http::Request &req,
                                            photon::net::http::Response &resp) {

        }

    }
}