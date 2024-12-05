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

/*
This module implements a generic framework that enables concurrent
out-of-ordering exeuction of *asynchronous* operations, while providing
a simple *synchronous* interface.

This framework supports potentially any asynchronous operation, by
dividing an operation into 3 parts: issue, wait for completion and
collection of result. The first 2 parts are realized via callbacks.
*/

#pragma once
#include <cinttypes>
#include <photon/common/callback.h>

namespace photon{

struct thread;

namespace rpc {


    class OutOfOrder_Execution_Engine;

    OutOfOrder_Execution_Engine* new_ooo_execution_engine();

    void delete_ooo_execution_engine(OutOfOrder_Execution_Engine* engine);

    struct OutOfOrderContext
    {
        OutOfOrder_Execution_Engine* engine;

        // an unique tag of the opeartion, which can be filled
        // by user (together with `flag_tag_valid` = true),
        // by the `engine`, or by `do_completion`.
        uint64_t tag;

        // The `CallbackType` have an prototype of
        // either `int (*)(void*, OutOfOrderContext*)`,
        // or     `int (CLAZZ::*)(OutOfOrderContext*)`.
        typedef Callback<OutOfOrderContext*> CallbackType;

        // The callback to issue an asynchronous operation, with
        // a tag specified in the argument. The tag should be retrieved
        // when the operation completes.
        // It's guaranteed not to be called concurrently.
        CallbackType do_issue;

        // The callback to do a blocking wait for the completion of any
        // issued operations, storing its *tag* to the `tag` field of the
        // provided `OutOfOrderContext` argument. After a successful return,
        // it's guaranteed not to be called again before `ooo_result_collected()`.
        // It's guaranteed not to be called concurrently.
        CallbackType do_completion;

        // The callback to do a blocking wait for the completion of any
        // issued operations, storing its *tag* to the `tag` field of the
        // provided `OutOfOrderContext` argument. After a successful return,
        // it's guaranteed not to be called again before `ooo_result_collected()`.
        // It's guaranteed not to be called concurrently.
        CallbackType do_collect;

        // whether or not the `tag` field is valid
        bool flag_tag_valid = false;

        // thread that binding with this argument
        thread * th;

        // whether the context result is collected
        bool collected;

        // return value of collection
        int ret;
    };


    // Issue an asynchronous operation,
    // storing it's *tag* to args if (!args.flag_tag_valid).
    // return 0 for success, negative for failure
    // Arguments: engine, do_issue, [tag, flag_tag_valid]
    extern "C" int ooo_issue_operation(OutOfOrderContext& args);

    // Wait for the completion of the operation.
    // returns 0 for success, negative for failures
    // if returns -2 and errno == ENOENT, there is a completed
    // operation but there is no caller in the registry to
    // collect the result, so users have to fix it up.
    // Arguments: engine, do_issue, [tag, flag_tag_valid], do_completion
    extern "C" int ooo_wait_completion(OutOfOrderContext& args);

    // Issue and operation and wait for its completion.
    // Return values are defined the same as above
    // Arguments: engine, do_completion
    extern "C" int ooo_issue_wait(OutOfOrderContext& args);

    // Inform the engine that the result has been colleted,
    // so that the engine can goes on.
    // Arguments: engine
    extern "C" void ooo_result_collected(OutOfOrderContext& args);

    // return concurrent task num of ooo engine
    extern "C" int ooo_get_queue_count(OutOfOrder_Execution_Engine* engine);

    // an exmaple on usage of the ooo engine
    inline void ooo_execution_example()
    {
        class Example
        {
        public:
            Example()
            {
                m_engine = new_ooo_execution_engine();
            }
            uint64_t OOO_Operation()
            {
                OutOfOrderContext args;
                init_ooo_args(&args);
                ooo_issue_wait(args);
                uint64_t ret = this->collect_result();
                ooo_result_collected(args);
                return ret;
            }
            ~Example()
            {
                delete_ooo_execution_engine(m_engine);
            }

        private:
            OutOfOrder_Execution_Engine* m_engine;
            void init_ooo_args(OutOfOrderContext* args)
            {
                args->engine = m_engine;
                args->do_issue.bind(this, &Example::issue);
                args->do_completion.bind(this, &Example::complete);
            }
            int issue(OutOfOrderContext* args)
            {   // issue the async operation with a tag of `args->tag`
                return 0;
            }
            int complete(OutOfOrderContext* args)
            {   // wait for a result of any issued async operation,
                // and store its tag to `args->tag`.
                return 0;
            }
            uint64_t collect_result()
            {   // collect and return the result
                return UINT64_MAX;
            }
        };

        Example().OOO_Operation();
    }
}
}