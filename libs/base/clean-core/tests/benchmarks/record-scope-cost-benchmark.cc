// What a recorded scope costs, beside the clocks a scope could read to tell running time from wall time.
//
// A scope's wall time cannot tell a thread that ran from one the OS kept off the CPU.
// A per-thread CPU clock read at both ends would, so this measures each candidate against the scope it would join:
// the thread's cycle count (QueryThreadCycleTime) and times (GetThreadTimes) on Windows, CLOCK_THREAD_CPUTIME_ID elsewhere,
// and the timestamp counter the recorder already reads, as the floor.

#include <clean-core/common/time.hh>
#include <clean-core/record/record.hh>
#include <clean-core/record/scope.hh>
#include <clean-core/record/system.hh>
#include <nexus/bench/barriers.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>

#ifdef CC_OS_WINDOWS
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
extern "C" __declspec(dllimport) int __stdcall QueryThreadCycleTime(void* thread, unsigned long long* cycles);
extern "C" __declspec(dllimport) int __stdcall GetThreadTimes(void* thread,
                                                              void* creation,
                                                              void* exit,
                                                              void* kernel,
                                                              void* user);
#else
#include <ctime>
#endif

using namespace cc::primitive_defines;

namespace
{
struct bench_recorder
{
    explicit bench_recorder(cc::rec::config cfg) { cc::rec::initialize(cfg); }
    ~bench_recorder() { cc::rec::shutdown(); }

    bench_recorder(bench_recorder const&) = delete;
    bench_recorder& operator=(bench_recorder const&) = delete;
};

constexpr auto run_cfg = nx::bench::run_config{.min_time_secs = 0.2, .max_samples = 2048};
} // namespace

BENCHMARK("bench-rec-scope-cost - a recorded scope against per-thread CPU clocks")
{
    {
        // the recorder running as a profile runs it, a consumer thread draining the scopes
        auto cfg = cc::rec::config{};
        cfg.overflow = cc::rec::overflow_policy::grow_unbounded;
        bench_recorder const recorder(cfg);
        (void)nx::bench::run("CC_RECORD_SCOPE, recording", run_cfg,
                             [&](isize count)
                             {
                                 for (isize i = 0; i < count; ++i)
                                 {
                                     CC_RECORD_SCOPE("bench-scope");
                                     nx::bench::sink(i);
                                 }
                             });
    }
    (void)nx::bench::run("cc::current_cycles, the timestamp counter", run_cfg,
                         [&](isize count)
                         {
                             for (isize i = 0; i < count; ++i)
                                 nx::bench::sink(cc::current_cycles());
                         });
#ifdef CC_OS_WINDOWS
    auto const thread = GetCurrentThread();
    (void)nx::bench::run("QueryThreadCycleTime", run_cfg,
                         [&](isize count)
                         {
                             for (isize i = 0; i < count; ++i)
                             {
                                 auto c = 0ull;
                                 (void)QueryThreadCycleTime(thread, &c);
                                 nx::bench::sink(c);
                             }
                         });
    (void)nx::bench::run("GetThreadTimes", run_cfg,
                         [&](isize count)
                         {
                             for (isize i = 0; i < count; ++i)
                             {
                                 unsigned long long t[4] = {};
                                 (void)GetThreadTimes(thread, &t[0], &t[1], &t[2], &t[3]);
                                 nx::bench::sink(t[3]);
                             }
                         });
#else
    (void)nx::bench::run("clock_gettime(CLOCK_THREAD_CPUTIME_ID)", run_cfg,
                         [&](isize count)
                         {
                             for (isize i = 0; i < count; ++i)
                             {
                                 auto ts = timespec{};
                                 (void)clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
                                 nx::bench::sink(ts.tv_nsec);
                             }
                         });
#endif
}
