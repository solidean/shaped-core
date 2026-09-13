#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/common/time.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_bound_scheduler.hh>
#include <nexus/test.hh>

#if CC_HAS_THREADS
#include <chrono>
#include <thread>
#endif

using namespace cc::primitive_defines;

// Homes: a node pinned to a scheduler runs every segment of its frame there.
// Most of these need a second thread to be a home at all, so they are gated; the main-home tests run in both threading modes.
// Every test that installs a compute scheduler holds the same tag as the pool tests, since the slot is process-wide.

namespace
{
u64 thread_id_now()
{
    return u64(cc::current_thread_id());
}

#if CC_HAS_THREADS
/// A thread that owns a home and pumps it until stopped.
/// Nodes homed to it must be dropped before it is, or the home's destructor asserts.
struct home_thread
{
    cc::thread_bound_scheduler* home = nullptr;
    cc::atomic<u64> id = {0};
    cc::atomic<bool> stop = {false};
    std::thread thread;

    home_thread()
    {
        cc::atomic<bool> ready = {false};
        thread = std::thread(
            [this, &ready]
            {
                cc::thread_bound_scheduler h;
                h.bind_to_current_thread();
                home = &h;
                id.store(thread_id_now());
                ready.store(true);
                while (!stop.load())
                {
                    (void)h.pump_cycle();
                    h.wait_for_work(1.0);
                }
                while (h.pump_cycle())
                {
                }
            });
        while (!ready.load())
            std::this_thread::yield();
    }

    ~home_thread()
    {
        stop.store(true);
        thread.join();
    }

    home_thread(home_thread const&) = delete;
    home_thread& operator=(home_thread const&) = delete;
};

/// A dependency that finishes on the pool after a short sleep, so whatever awaits it genuinely parks and is woken off-home.
cc::shared_async<int> slow_on_pool(cc::async_thread_pool& pool, int value)
{
    auto node = cc::make_async_lazy(
        [value]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return value;
        });
    node->schedule_on(pool);
    return node;
}

cc::shared_async<int> hop_and_record(cc::async_scheduler& home,
                                     cc::async_thread_pool& pool,
                                     cc::atomic<u64>& before,
                                     cc::atomic<u64>& after)
{
    co_await cc::async_resume_on(home);
    before.store(thread_id_now());
    auto const first = slow_on_pool(pool, 20); // named: a reference into an awaited temporary dangles
    auto const second = slow_on_pool(pool, 22);
    auto const a = co_await first;
    auto const b = co_await second;
    after.store(thread_id_now());
    co_return a + b;
}
#endif
} // namespace

#if CC_HAS_THREADS

TEST("async home - every segment of a hopped coroutine runs on its home, across wakes from the pool",
     nx::config::no_scheduler,
     exclusive("cc-compute-async-pool"))
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);
    home_thread h;

    cc::atomic<u64> before = {0};
    cc::atomic<u64> after = {0};
    {
        auto const root = hop_and_record(*h.home, pool, before, after);
        CHECK(cc::async_blocking_get(root) == 42);
    }

    CHECK(before.load() == h.id.load());
    CHECK(after.load() == h.id.load()); // woken twice by pool workers, and still resumed at home
    CHECK(h.home->homed_node_count() == 0);
}

TEST("async home - a homed factory node runs its frame on its home, whoever drives it",
     nx::config::no_scheduler,
     exclusive("cc-compute-async-pool"))
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);
    home_thread h;

    cc::atomic<u64> ran_on = {0};
    {
        auto const dep = slow_on_pool(pool, 41);
        auto const homed = cc::make_async_lazy_on(
            *h.home,
            [&ran_on](int x)
            {
                ran_on.store(thread_id_now());
                return x + 1;
            },
            dep);

        // Driving it on a pool means waiting for its home: the pool never runs it itself.
        CHECK(cc::async_blocking_get_on(pool, homed) == 42);
    }
    CHECK(ran_on.load() == h.id.load());
}

TEST("async home - a pool never drives a homed dependency inline",
     nx::config::no_scheduler,
     exclusive("cc-compute-async-pool"))
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);
    home_thread h;

    cc::atomic<u64> dep_ran_on = {0};
    cc::atomic<u64> root_ran_on = {0};
    {
        auto const homed_dep = cc::make_async_lazy_on(*h.home,
                                                      [&dep_ran_on]
                                                      {
                                                          dep_ran_on.store(thread_id_now());
                                                          return 5;
                                                      });
        auto const root = cc::make_async_lazy(
            [&root_ran_on](int x)
            {
                root_ran_on.store(thread_id_now());
                return x * 2;
            },
            homed_dep);

        CHECK(cc::async_blocking_get_on(pool, root) == 10);
    }
    CHECK(dep_ran_on.load() == h.id.load());
    CHECK(root_ran_on.load() != h.id.load()); // the unhomed root is not pulled onto the home by its dependency
}

TEST("async home - same_home_only sends an unhomed cold dependency to compute, any drives it inline",
     nx::config::no_scheduler,
     exclusive("cc-compute-async-pool"))
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);
    home_thread h;

    auto const run = [&](cc::async_inline_deps policy)
    {
        cc::atomic<u64> dep_ran_on = {0};
        {
            auto const dep = cc::make_async_lazy(
                [&dep_ran_on]
                {
                    dep_ran_on.store(thread_id_now());
                    return 3;
                });
            auto const homed = cc::make_async_lazy_on(*h.home, {.inline_deps = policy}, [](int x) { return x + 1; }, dep);
            CHECK(cc::async_blocking_get_on(pool, homed) == 4);
        }
        return dep_ran_on.load();
    };

    CHECK(run(cc::async_inline_deps::same_home_only) != h.id.load());
    CHECK(run(cc::async_inline_deps::home_default) != h.id.load()); // a thread home's default is same_home_only
    CHECK(run(cc::async_inline_deps::any) == h.id.load());
}

TEST("async home - work a homed body starts is not homed, and never lands on the home",
     nx::config::no_scheduler,
     exclusive("cc-compute-async-pool"))
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);
    home_thread h;

    cc::atomic<u64> child_ran_on = {0};
    {
        auto const homed = cc::make_async_lazy_on(*h.home,
                                                  [&child_ran_on]
                                                  {
                                                      return cc::make_async_scheduled(
                                                          [&child_ran_on]
                                                          {
                                                              child_ran_on.store(thread_id_now());
                                                              return 1;
                                                          });
                                                  });
        auto const child = cc::async_blocking_get(homed);
        CHECK(cc::async_blocking_get(child) == 1);
    }
    CHECK(child_ran_on.load() != 0);
    CHECK(child_ran_on.load() != h.id.load());
}

TEST("async home - a thread blocked on a graph still runs the steps homed to it",
     nx::config::no_scheduler,
     exclusive("cc-compute-async-pool"))
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);

    // The owner itself blocks: the homed step in the middle of its graph can only run if the wait services the home.
    cc::atomic<int> result = {0};
    cc::atomic<u64> step_ran_on = {0};
    cc::atomic<u64> owner = {0};
    std::thread t(
        [&]
        {
            cc::thread_bound_scheduler home;
            home.bind_to_current_thread();
            owner.store(thread_id_now());
            {
                auto const first = slow_on_pool(pool, 20);
                auto const homed = cc::make_async_lazy_on(
                    home,
                    [&step_ran_on](int x)
                    {
                        step_ran_on.store(thread_id_now());
                        return x + 1;
                    },
                    first);
                auto const last = cc::make_async_lazy([](int x) { return x * 2; }, homed);
                result.store(cc::async_blocking_get(last));
            }
        });
    t.join();

    CHECK(result.load() == 42);
    CHECK(step_ran_on.load() == owner.load());
}

TEST("async home - an at_home teardown of an abandoned frame runs on the home", nx::config::no_scheduler)
{
    home_thread h;

    struct records_destruction
    {
        cc::atomic<u64>* where;
        records_destruction(cc::atomic<u64>* w) : where(w) {}
        records_destruction(records_destruction&& r) noexcept : where(r.where) { r.where = nullptr; }
        ~records_destruction()
        {
            if (where != nullptr)
                where->store(thread_id_now());
        }
    };

    cc::atomic<u64> destroyed_on = {0};
    {
        auto node = cc::make_async_lazy_on(*h.home, {.teardown = cc::async_teardown::at_home},
                                           [r = records_destruction(&destroyed_on)] { return 1; });
        CHECK(h.home->homed_node_count() == 1);
        node = nullptr; // dropped cold, on this thread: the capture must not be released here
    }

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // The capture is released inside the deferred teardown, and the home's count drops only after it returns.
    while ((destroyed_on.load() == 0 || h.home->homed_node_count() != 0) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();

    CHECK(destroyed_on.load() == h.id.load());
    CHECK(h.home->homed_node_count() == 0);
}

TEST("async home - an anywhere teardown releases the frame where the last handle drops", nx::config::no_scheduler)
{
    home_thread h;

    cc::atomic<u64> destroyed_on = {0};
    struct records_destruction
    {
        cc::atomic<u64>* where;
        records_destruction(cc::atomic<u64>* w) : where(w) {}
        records_destruction(records_destruction&& r) noexcept : where(r.where) { r.where = nullptr; }
        ~records_destruction()
        {
            if (where != nullptr)
                where->store(thread_id_now());
        }
    };

    {
        auto node = cc::make_async_lazy_on(*h.home, [r = records_destruction(&destroyed_on)] { return 1; });
        node = nullptr;
    }
    CHECK(destroyed_on.load() == thread_id_now());
    CHECK(h.home->homed_node_count() == 0);
}

TEST("async home - a hop moves a coroutine between homes and keeps it on the last one",
     nx::config::no_scheduler,
     exclusive("cc-compute-async-pool"))
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);
    home_thread h1;
    home_thread h2;

    cc::atomic<u64> on_first = {0};
    cc::atomic<u64> on_second = {0};
    cc::atomic<u64> after_wake = {0};

    auto const body = [&]() -> cc::shared_async<int>
    {
        co_await cc::async_resume_on(*h1.home);
        on_first.store(thread_id_now());
        co_await cc::async_resume_on(*h2.home);
        on_second.store(thread_id_now());
        auto const dep = slow_on_pool(pool, 7);
        auto const v = co_await dep;
        after_wake.store(thread_id_now());
        co_return v;
    };

    {
        auto const root = body();
        CHECK(cc::async_blocking_get(root) == 7);
    }
    CHECK(on_first.load() == h1.id.load());
    CHECK(on_second.load() == h2.id.load());
    CHECK(after_wake.load() == h2.id.load());
    CHECK(h1.home->homed_node_count() == 0);
    CHECK(h2.home->homed_node_count() == 0);
}

TEST("async home - async_run_on runs a section elsewhere and hands its value back",
     nx::config::no_scheduler,
     exclusive("cc-compute-async-pool"))
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);
    home_thread h;

    cc::atomic<u64> section_ran_on = {0};
    cc::atomic<u64> body_ran_on = {0};
    auto const body = [&]() -> cc::shared_async<cc::string>
    {
        co_await cc::async_resume_on(*h.home);
        auto text = co_await cc::async_run_on(pool,
                                              [&section_ran_on]
                                              {
                                                  section_ran_on.store(thread_id_now());
                                                  return cc::string("computed");
                                              });
        body_ran_on.store(thread_id_now());
        co_return cc::move(text);
    };

    {
        auto const root = body();
        CHECK(cc::async_blocking_get(root) == "computed");
    }
    CHECK(section_ran_on.load() != h.id.load());
    CHECK(body_ran_on.load() == h.id.load()); // the section did not move the body's home
}

TEST("async home - a home re-queues itself only behind what one pump cycle already holds", nx::config::no_scheduler)
{
    home_thread h;

    cc::atomic<int> polls = {0};
    cc::atomic<bool> release = {false};
    {
        auto const spinner = [&]() -> cc::shared_async<cc::unit>
        {
            co_await cc::async_resume_on(*h.home);
            while (!release.load())
            {
                polls.fetch_add(1);
                co_await cc::async_yield();
            }
            co_return;
        };

        auto const root = spinner();
        root->schedule_on(*h.home); // a cold coroutine reaches its home by being scheduled there; its hop then keeps it

        auto const busy_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (polls.load() < 100 && std::chrono::steady_clock::now() < busy_deadline)
            std::this_thread::yield();
        CHECK(polls.load() >= 100); // the owner keeps getting through its loop, and so is not pinned by the spinner
        release.store(true);

        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!root->is_ready() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        CHECK(root->is_ready());
    }
    CHECK(polls.load() > 0);
    CHECK(h.home->homed_node_count() == 0);
}

#endif // CC_HAS_THREADS

// ---- the main home: these run in every threading mode ----

TEST("async home - a main-homed node completes through pump_main_thread", nx::config::main_thread)
{
    auto& main = cc::main_thread_scheduler();
    CHECK(main.is_owner_thread());

    cc::atomic<u64> ran_on = {0};
    {
        auto const node = cc::make_async_scheduled_on_main(
            [&ran_on]
            {
                ran_on.store(thread_id_now());
                return 9;
            });

        // Bounded by time, not by rounds: the test runs beside the shared phase, so the machine may be busy.
        auto const deadline = cc::current_time_steady_secs() + 10.0;
        while (!node->is_ready() && cc::current_time_steady_secs() < deadline)
            (void)cc::pump_main_thread(1.0);

        REQUIRE(node->is_ready());
        CHECK(node->value() == 9);
    }
    CHECK(ran_on.load() == u64(cc::thread_id::main));
    CHECK(main.homed_node_count() == 0);
}

TEST("async home - a main thread blocked on a graph runs its main-homed steps", nx::config::main_thread)
{
    cc::atomic<u64> ran_on = {0};
    auto const body = [&]() -> cc::shared_async<int>
    {
        auto const dep = cc::make_async_scheduled([] { return 20; });
        auto const first = co_await dep;
        co_await cc::async_resume_on_main();
        ran_on.store(thread_id_now());
        co_return first + 22;
    };

    {
        auto const root = body();
        CHECK(cc::async_blocking_get(root) == 42);
    }
    CHECK(ran_on.load() == u64(cc::thread_id::main));
    CHECK(cc::main_thread_scheduler().homed_node_count() == 0);
}

TEST("async home - a yielding main-homed body does not pin pump_main_thread", nx::config::main_thread)
{
    cc::atomic<int> polls = {0};
    auto const spinner = [&]() -> cc::shared_async<cc::unit>
    {
        co_await cc::async_resume_on_main();
        while (polls.load() < 50)
        {
            polls.fetch_add(1);
            co_await cc::async_yield();
        }
        co_return;
    };

    {
        auto const root = cc::async_start(spinner());
        auto const deadline = cc::current_time_steady_secs() + 10.0;

        // Its first segment runs wherever async_start sent it; pump until it has hopped home and started spinning.
        while (polls.load() == 0 && cc::current_time_steady_secs() < deadline)
            (void)cc::pump_main_thread();
        REQUIRE(polls.load() > 0);

        // One cycle runs what was queued when it started, and a yield re-queues behind that snapshot.
        // The sweep inside pump_main_thread runs the home a second time, so one call may take two segments, never the rest.
        auto const before = polls.load();
        (void)cc::pump_main_thread();
        CHECK(polls.load() - before <= 2);

        while (!root->is_ready() && cc::current_time_steady_secs() < deadline)
            (void)cc::pump_main_thread();
        CHECK(root->is_ready());
    }
    CHECK(polls.load() == 50);
}
