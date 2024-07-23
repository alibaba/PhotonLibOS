//
// Created by jun on 2024/7/23.
//

#ifndef PHOTON_XYHTTP_H
#define PHOTON_XYHTTP_H

#include "photon/net/http/server.h"


namespace ZyIo{
    namespace XyHttp {

        class XyHttpFilter{
        private:
            int order;
            std::string_view pattern;
            XyHttpFilter() = delete;
        public:
            XyHttpFilter(int order,std::string_view pattern);

            /**
             * if return true,the filter keep run
             * if return false,the filter will stop,the biz handle logic will not be execute
             * @param req
             * @param resp
             * @return
             */
            bool preHandle(photon::net::http::Request& req, photon::net::http::Response& resp);

            void postHandle(photon::net::http::Request& req, photon::net::http::Response& resp);

            void afterHandle(photon::net::http::HTTPHandler ,photon::net::http::Request& req, photon::net::http::Response& resp);


        };

        class XyHttpServer : public photon::net::http::HTTPServer{
        public:
        };

    };
}



#endif //PHOTON_XYHTTP_H
