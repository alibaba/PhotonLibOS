//
// Created by jun on 24-9-4.
//

#ifndef PHOTON_TOOLS_H
#define PHOTON_TOOLS_H
#include <queue>
#include <vector>

namespace zyio{

    namespace tools{

        class Delayed{
        public:
            virtual long delay() const = 0;
        };


        struct CustomCompare {
            bool operator()(const Delayed& a, const Delayed& b) {
                // 自定义排序规则，按照元素的value进行排序
                return a.delay() > b.delay();
            }
        };


        template <typename T,typename = std::enable_if_t<std::is_base_of_v<Delayed, T>>>
        class DelayQueue {
        private:
            std::priority_queue<T,std::vector<T>,CustomCompare> minHeap;

        public:

            void push(T t){
                minHeap.push(t);
            }

            T top(){
                return minHeap.top();
            }

            void pop(){
                minHeap.pop();
            }

            bool empty(){
                return minHeap.empty();
            }
        };



    }
}



#endif //PHOTON_TOOLS_H
