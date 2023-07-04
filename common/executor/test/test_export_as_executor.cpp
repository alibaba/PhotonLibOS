#include <gtest/gtest.h>
#include <photon/common/executor/executor.h>
#include <photon/common/utility.h>
#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <thread>

TEST(enter_as_executor, test) {
    // set global default logger output to null
    set_log_output(log_output_null);

    // create own logger
    ALogLogger logger;
    // set log_output to stdout, log level as info
    logger.log_output = log_output_stdout;
    logger.log_level = ALOG_INFO;

    photon::init();
    DEFER(photon::fini());
    // do some ready work in this photon environment
    auto vcpu = photon::get_vcpu();
    auto e = photon::Executor::export_as_executor();
    DEFER(delete e);

    std::thread([&logger, e, vcpu] {
        e->perform([&logger, vcpu] {
            logger << LOG_INFO("Hello from a normal thread");
            auto cvcpu = photon::get_vcpu();
            logger << LOG_INFO("executor at `, current on `, `", vcpu, cvcpu,
                               vcpu == cvcpu ? "EQ" : "NEQ");
        });
    }).detach();

    photon::thread_usleep(1UL * 1024 * 1024);
}