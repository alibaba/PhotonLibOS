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
         *
         * The Node encapsulates both waiting and notification logic,
         * supporting two synchronization modes:
         * - Yield-based: Uses done_flag for cooperative waiting
         * - Semaphore-based: Uses semaphore for blocking wait
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

            /**
             * @brief Wait for execution to complete
             *
             * Blocks the current thread until the executor completes and
             * notifies. The waiting strategy is determined by use_sem:
             * - If use_sem=false: Spin-yields checking done_flag
             * - If use_sem=true: Blocks on semaphore wait
             *
             * @note After this method returns, the result (if any) is
             * guaranteed to be written to result_ptr
             */
            void wait() {
                if (use_sem) {
                    sem.wait(1);
                } else {
                    while (!done_flag.load(std::memory_order_acquire)) {
                        photon::thread_yield();
                    }
                }
            }

            /**
             * @brief Write result and notify waiting thread
             *
             * Called by the executor to deliver the result and wake up this
             * waiting node. Uses different notification mechanisms based on
             * the waiting mode:
             * - If use_sem=false: Sets done_flag with release semantics
             * - If use_sem=true: Only signals the semaphore (skip done_flag)
             *
             * @tparam T Result type
             * @param result Pointer to result object (nullptr if no result)
             *
             * @note ORDERING and safety:
             * 1. Write result (if applicable)
             * 2. Semaphore mode: Signal semaphore (waiter blocked, won't destruct)
             *    Yield mode: Set done_flag (this is the LAST access to Node)
             *
             * @note Performance: Semaphore mode skips done_flag modification to
             * avoid unnecessary atomic operation and memory barrier, since
             * semaphore provides sufficient synchronization on its own.
             *
             * @note Safety: Both branches are safe without pre-reading use_sem:
             * - Semaphore branch: Waiter blocked at sem.wait(), Node won't
             *   destruct until sem.signal() completes
             * - Yield branch: done_flag.store() is the last Node access before
             *   waiter may wake up and destruct
             */
            template <typename T>
            void set_result_and_notify(const T* result) {
                if (result && result_ptr) {
                    *(T*)result_ptr = *result;
                }
                if (use_sem) {
                    // Semaphore mode: signal directly without touching done_flag
                    // The semaphore provides sufficient synchronization
                    sem.signal(1);
                } else {
                    // Yield mode: set done_flag to release spin-waiting thread
                    // This is the last access to Node before waiter may destruct
                    done_flag.store(true, std::memory_order_release);
                }
            }
        };

        std::atomic<Node*> head{};  ///< Head pointer of linked list, using
                                    ///< atomic operations for thread safety

        /**
         * @brief Register node and determine current thread's role
         *
         * Atomically inserts the current thread's node at the head of the
         * waiting list using lock-free CAS operations. The first thread to
         * successfully insert with next == nullptr becomes the executor,
         * while subsequent threads become waiters.
         *
         * @param node Reference to the Node object on the caller's stack
         * @return true if current thread is a waiter (should wait for result),
         *         false if it's the executor (should execute func)
         *
         * @note Thread-safe: Uses CAS with memory_order_acq_rel for
         * synchronization
         * @note The Node must remain valid on the stack until this method
         * returns (for executors) or until wait() returns (for waiters)
         * @note Uses photon::spin_wait() for backoff when CAS contention occurs
         */
        bool ready(Node& node) {
            node.next = head.load(std::memory_order_acquire);
            while (!head.compare_exchange_weak(node.next, &node,
                                               std::memory_order_acq_rel))
                photon::spin_wait();
            if (node.next) node.wait();
            return node.next != nullptr;
        }

        /**
         * @brief Notify all waiting threads with the execution result
         *
         * Called by the executor thread after completing func execution.
         * Atomically detaches the entire waiting list and iterates through
         * each node to deliver the result and wake up waiting threads.
         *
         * Process:
         * 1. Atomically swap head with nullptr (detach entire list)
         * 2. For each node in the detached list:
         *    - Call set_result_and_notify() to write result and wake the waiter
         * 3. Continue until all nodes are processed
         *
         * @tparam T Result type
         * @param result Pointer to the result object (nullptr for void return)
         *
         * @note Memory safety: Uses exchange() to ensure no new nodes can be
         * added after detachment, preventing use-after-free
         * @note All waiters are guaranteed to see the result due to
         * release-acquire semantics in set_result_and_notify()
         * @note CRITICAL: Must save n->next BEFORE calling set_result_and_notify()
         * because the Node may be destructed immediately after notification
         */
        template <typename T>
        void done(const T* result = nullptr) {
            auto n = head.exchange(nullptr, std::memory_order_acq_rel);
            while (n) {
                // CRITICAL: Save next pointer before notifying
                // After set_result_and_notify(), the waiter may wake up and
                // destruct the Node, making n->next invalid
                auto next = n->next;
                n->set_result_and_notify(result);
                n = next;  // Safe: using saved value, not accessing freed memory
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