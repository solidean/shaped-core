// Home wake latency: how long a node homed to a thread takes from "made runnable" to "done", measured from the thread that made it runnable.
// It is the cost the main thread pays per homed step, which is what decides how chatty a main-thread coroutine may be.
// The numbers and what they are compared against are in libs/base/clean-core/docs/benchmarks/async-benchmark.md.

#include <clean-core/common/macros.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread_bound_scheduler.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>

#if CC_HAS_THREADS
#include <thread>

using cc::i64;

namespace
{
/// An owner thread for a home, parked between items — the realistic case for a loop waiting on events.
/// Not a spinning owner: pumping without ever waiting contends on the queue mutex with every submit, which measures the spin rather than the wake.
struct bench_home
{
    cc::thread_bound_scheduler* home = nullptr;
    cc::atomic<bool> stop = {false};
    std::thread thread;

    bench_home()
    {
        cc::atomic<bool> ready = {false};
        thread = std::thread(
            [this, &ready]
            {
                cc::thread_bound_scheduler h;
                h.bind_to_current_thread();
                home = &h;
                ready.store(true);
                while (!stop.load(cc::memory_order_relaxed))
                    if (!h.pump_cycle())
                        h.wait_for_work(1.0);
                while (h.pump_cycle())
                {
                }
            });
        while (!ready.load())
            std::this_thread::yield();
    }

    ~bench_home()
    {
        stop.store(true);
        thread.join();
    }
};

void wait_ready(cc::async_node_base const& n)
{
    while (!n.is_ready())
        std::this_thread::yield();
}
} // namespace

BENCHMARK("bench-async-home - wake latency")
{
    constexpr auto cfg = nx::bench::run_config{.min_time_secs = 0.2, .max_samples = 2048};

    {
        bench_home parked;
        (void)nx::bench::run("submit to a parked home", cfg,
                             [&]
                             {
                                 auto const n = cc::make_async_scheduled_on(*parked.home, [] { return i64(1); });
                                 wait_ready(*n);
                                 nx::bench::sink(n->value());
                             });
    }

    {
        // The reference: the same node, unhomed, submitted to a one-worker pool from a foreign thread.
        cc::async_thread_pool pool(1);
        (void)nx::bench::run("submit to a 1-worker pool (unhomed)", cfg,
                             [&]
                             {
                                 auto const n = cc::make_async_lazy([] { return i64(1); });
                                 n->schedule_on(pool);
                                 wait_ready(*n);
                                 nx::bench::sink(n->value());
                             });
    }
}
#endif
