/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <photon/thread/thread.h>

#include <atomic>
#include <utility>

namespace photon {

/**
 * @brief SingleFlight - Request coalescing utility for multi-threaded
 * environments
 *
 * When multiple threads make the same request simultaneously, ensures that only
 * one actual operation is executed, while other threads wait and share the
 * result from the first thread.
 *
 * Typical use cases:
 * - Cache stampede prevention: When multiple requests query an uncached key
 * simultaneously, execute only one database query
 * - Duplicate computation elimination: Multiple threads requesting the same
 * expensive computation
 * - API rate limiting optimization: Coalesce identical API requests within a
 * short time window
 *
 * Usage examples:
 * @code
 * photon::SingleFlight sf;
 *
 * // Example 1: Short operation with yield-based waiting (default)
 * auto result = sf.Do([&]() {
 *     return quick_operation();  // Executes only once
 * });
 *
 * // Example 2: Long operation with semaphore-based waiting
 * auto data = sf.Do([&]() {
 *     return expensive_db_query();  // Long operation
 * }, true);  // use_sem = true for better CPU efficiency
 *
 * // Example 3: Void return type
 * sf.Do([&]() {
 *     process_data();
 * });
 *
 * // Multi-threaded scenario
 * std::vector<photon::thread*> threads;
 * for (int i = 0; i < 100; i++) {
 *     threads.push_back(photon::thread_create11([&]() {
 *         auto result = sf.Do([&]() {
 *             return expensive_operation();  // Executes only once
 *         });
 *         // All threads receive the same result
 *     }));
 * }
 * @endcode
 *
 * Performance characteristics:
 * - Lock-free design using CAS operations for high-performance concurrency
 * - Stack-allocated Node objects to avoid heap allocation overhead
 * - Flexible waiting strategies:
 *   * Yield-based (default): Better for short operations, lower latency
 *   * Semaphore-based: Better for long operations, reduces CPU usage
 *
 * Caveats:
 * - Exception handling is not supported; func should not throw exceptions
 * - Each SingleFlight instance works independently; different instances don't
 * share state
 * - Choose appropriate wait mode based on operation duration for optimal
 * performance
 */
class SingleFlight {
    /**
     * @brief Wait link - Core data structure for SingleFlight
     *
     * Manages waiting thread nodes using a lock-free linked list. The first
     * thread to successfully insert becomes the executor, while other threads
     * become waiters. After execution completes, the executor notifies all
     * waiters via done().
     */
    struct WaitLink {
        /**
         * @brief Linked list node, stored on thread stack
         *
         * Each thread calling Do() creates a Node on its stack,
         * with lifetime covering the entire Do() call.
         */
        struct Node {
            Node* next;        ///< Pointer to next node in the linked list
            void* result_ptr;  ///< Pointer to result storage location (address
                               ///< of stack variable)
            photon::semaphore sem;
            bool use_sem;  ///< If true, use semaphore for blocking wait;
                           ///< if false, use yield-based spinning
            std::atomic<bool> done_flag{
                false};  ///< Flag indicating execution completion

            Node(void* result_ptr, bool use_sem)
                : next(nullptr),
                  result_ptr(result_ptr),
                  sem(0),
                  use_sem(use_sem) {}
        };

        std::atomic<Node*> head{};  ///< Head pointer of linked list, using
                                    ///< atomic operations for thread safety

        /**
         * @brief Wait for execution to complete
         *
         * Waiting coroutines call this method, blocking until the executor
         * completes done(). Supports two wait modes:
         * - If use_sem is false: Uses photon::thread_yield() for cooperative
         * waiting (suitable for short operations, lower latency)
         * - If use_sem is true: Uses semaphore blocking (suitable for long
         * operations to reduce CPU usage)
         *
         * @param node Reference to the node containing wait state
         * @note Waits on done_flag to ensure result has been written before
         * returning
         */
        void __wait(Node& node) {
            if (node.use_sem) {
                node.sem.wait(1);
            } else {
                while (!node.done_flag.load(std::memory_order_acquire)) {
                    photon::thread_yield();
                }
            }
        }

        /**
         * @brief Register node and determine current coroutine's role
         *
         * Inserts the current coroutine's node at the head of the list
         * (stack-style insertion). The first coroutine to successfully insert
         * with next == nullptr becomes the executor, while other coroutines
         * become waiters and enter __wait().
         *
         * @param node Reference to the Node object on the stack
         * @return true if current coroutine is a waiter, false if it's the
         * executor
         *
         * @note This method uses CAS operations to ensure thread-safe list
         * insertion
         * @note The Node object must remain valid until __wait() returns
         * or execution completes
         * @note Uses photon::spin_wait() for brief spinning when CAS fails
         */
        bool ready(Node& node) {
            auto n = &node;
            n->next = head.load(std::memory_order_acquire);
            while (!head.compare_exchange_weak(n->next, n,
                                               std::memory_order_acq_rel))
                photon::spin_wait();
            if (n->next) __wait(node);
            return n->next != nullptr;
        }

        /**
         * @brief Complete execution and notify all waiters
         *
         * The executor coroutine calls this method to traverse all nodes in the
         * list and notify waiters:
         * 1. Load the entire waiting list atomically
         * 2. For each waiting node:
         *    a. Copy result to node's result_ptr (if applicable)
         *    b. Set done_flag to release yield-based waiters
         *    c. Signal semaphore to release semaphore-based waiters
         * 3. Clear the head pointer to nullptr
         *
         * @tparam T Result type
         * @param result Pointer to the result object (nullptr indicates no
         * return value)
         *
         * @note Critical ordering: result must be written and done_flag must be
         * set BEFORE signaling the semaphore, because once sem.signal() is
         * called, the waiting coroutine may wake up immediately and access the
         * result.
         * @note For yield-based waiters, done_flag ensures they see the result
         * before proceeding.
         */
        template <typename T>
        void done(const T* result = nullptr) {
            auto n = head.exchange(nullptr, std::memory_order_acq_rel);
            while (n) {
                auto next = n->next;
                if (result && n->result_ptr) {
                    *(T*)n->result_ptr = *result;
                }
                if (n->use_sem) {
                    n->sem.signal(1);
                } else {
                    n->done_flag.store(true, std::memory_order_release);
                }
                n = next;
            }
        }
    } wl;

public:
    /**
     * @brief Execute function (void return type version)
     *
     * @tparam Func Callable object type
     * @param func Function to execute, must return void
     * @param use_sem If true, waiting coroutines use semaphore blocking; if
     * false, use thread_yield()
     *
     * When multiple coroutines call simultaneously, only one coroutine will
     * execute func, while others wait until execution completes.
     *
     * @note For short operations, use default (use_sem=false) for better
     * performance
     * @note For long operations, set use_sem=true to reduce CPU usage during
     * waiting
     */
    template <typename Func,
              typename RetType = decltype(std::declval<Func>()())>
    std::enable_if_t<std::is_void<RetType>::value, void> Do(
        Func&& func, bool use_sem = false) {
        WaitLink::Node node(nullptr, use_sem);
        if (!wl.ready(node)) {
            func();
            wl.done<void*>();
        }
    }

    /**
     * @brief Execute function (with return value version)
     *
     * @tparam Func Callable object type
     * @param func Function to execute, with return type RetType
     * @param use_sem If true, waiting coroutines use semaphore blocking; if
     * false, use thread_yield()
     * @return RetType The function's return value
     *
     * When multiple coroutines call simultaneously, only one coroutine will
     * execute func, and all coroutines will receive the same return value (via
     * stack memory copy).
     *
     * @note RetType must be a copyable type
     * @note Return value is passed by value copy to avoid dangling references
     * @note For short operations, use default (use_sem=false) for better
     * performance
     * @note For long operations, set use_sem=true to reduce CPU usage during
     * waiting
     */
    template <typename Func,
              typename RetType = decltype(std::declval<Func>()())>
    std::enable_if_t<!std::is_void<RetType>::value, RetType> Do(
        Func&& func, bool use_sem = false) {
        RetType result;
        WaitLink::Node node(&result, use_sem);
        if (!wl.ready(node)) {
            result = func();
            wl.done(&result);
        }
        return result;
    }

    /**
     * @brief Destructor
     *
     * Waits for all pending operations to complete (head becomes nullptr).
     * Ensures no coroutines are using WaitLink during destruction.
     *
     * @note Uses photon::thread_yield() to avoid blocking system threads
     */
    ~SingleFlight() {
        while (wl.head.load(std::memory_order_acquire)) {
            photon::thread_yield();
        }
    }
};

}  // namespace photon