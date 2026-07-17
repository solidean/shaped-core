#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/to_string.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <nexus/test.hh>

#include <memory>

using namespace cc::primitive_defines;

// Most tests run the actor unthreaded and drive it by hand: deterministic, race-free, and valid on
// every platform (including single-threaded WebAssembly). A small threaded smoke section, gated on
// CC_HAS_THREADS, exercises the real background thread.

// ============================================================================
// Test actors
// ============================================================================

// Single-type actor logging int messages in order.
class int_log_actor : public cc::threaded_actor_impl<int>
{
public:
    cc::vector<int> log;
    cc::atomic<int> init_count{0};
    cc::atomic<int> shutdown_count{0};

protected:
    void on_thread_init() override { init_count++; }
    void on_thread_shutdown() override { shutdown_count++; }
    void on_message(int msg) override { log.push_back(msg); }
};

struct msg_a
{
    int id;
};
struct msg_b
{
    int id;
};
struct msg_c
{
    int id;
};

// Multi-type actor logging A and B with type tags.
class multi_type_actor : public cc::threaded_actor_impl<msg_a, msg_b>
{
public:
    struct entry
    {
        char type;
        int id;
    };
    cc::vector<entry> log;

protected:
    void on_message(msg_a m) override { log.push_back({'A', m.id}); }
    void on_message(msg_b m) override { log.push_back({'B', m.id}); }
};

// Three-type actor exercising variadic expansion.
class three_type_actor : public cc::threaded_actor_impl<msg_a, msg_b, msg_c>
{
public:
    cc::vector<char> tags;

protected:
    void on_message(msg_a) override { tags.push_back('A'); }
    void on_message(msg_b) override { tags.push_back('B'); }
    void on_message(msg_c) override { tags.push_back('C'); }
};

// Records init/shutdown ordering with sentinel markers (-1 init, -2 shutdown), messages as values.
class lifecycle_actor : public cc::threaded_actor_impl<int>
{
public:
    static constexpr int marker_init = -1;
    static constexpr int marker_shutdown = -2;
    cc::vector<int> log;

protected:
    void on_thread_init() override { log.push_back(marker_init); }
    void on_thread_shutdown() override { log.push_back(marker_shutdown); }
    void on_message(int msg) override { log.push_back(msg); }
};

// Batches messages into a local queue and flushes them out in on_process.
class batching_actor : public cc::threaded_actor_impl<int>
{
public:
    cc::vector<int> local_queue;
    std::shared_ptr<cc::vector<int>> output;
    cc::atomic<int> process_calls{0};

    explicit batching_actor(std::shared_ptr<cc::vector<int>> out) : output(std::move(out)) {}

protected:
    void on_message(int msg) override { local_queue.push_back(msg); }

    bool on_process() override
    {
        process_calls++;
        if (local_queue.empty())
            return false;
        for (int v : local_queue)
            output->push_back(v);
        local_queue.clear();
        return true;
    }
};

// Forwards ints to a downstream int actor.
class forwarding_actor : public cc::threaded_actor_impl<int>
{
public:
    cc::vector<int> log;
    cc::threaded_actor<int>* next = nullptr;

    explicit forwarding_actor(cc::threaded_actor<int>* n) : next(n) {}

protected:
    void on_message(int msg) override
    {
        log.push_back(msg);
        if (next)
            next->enqueue_message(msg);
    }
};

// Converts ints to strings and forwards.
class int_to_string_actor : public cc::threaded_actor_impl<int>
{
public:
    cc::vector<int> log;
    cc::threaded_actor<cc::string>* next = nullptr;

    explicit int_to_string_actor(cc::threaded_actor<cc::string>* n) : next(n) {}

protected:
    void on_message(int msg) override
    {
        log.push_back(msg);
        if (next)
            next->enqueue_message(cc::to_string(msg));
    }
};

// String sink.
class string_log_actor : public cc::threaded_actor_impl<cc::string>
{
public:
    std::shared_ptr<cc::vector<cc::string>> log;

    explicit string_log_actor(std::shared_ptr<cc::vector<cc::string>> l) : log(std::move(l)) {}

protected:
    void on_message(cc::string msg) override { log->push_back(cc::move(msg)); }
};

namespace
{
// Drive an unthreaded actor until it has no more work. Test agents go idle, so this terminates.
template <class Actor>
void pump_until_idle(Actor& actor)
{
    while (actor.process_messages_if_unthreaded())
    {
    }
}
constexpr auto unthreaded = cc::threaded_actor_mode::unthreaded;
} // namespace

// ============================================================================
// Unthreaded, deterministic (all platforms)
// ============================================================================

TEST("threaded_actor - unthreaded single-type in-order delivery")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();
    actor->start(unthreaded);

    for (int i = 0; i < 5; ++i)
        REQUIRE(actor->enqueue_message(i));
    pump_until_idle(*actor);
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == 5);
    for (int i = 0; i < 5; ++i)
        CHECK(impl->log[i] == i);
}

TEST("threaded_actor - unthreaded pre-start messages are kept")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();

    REQUIRE(actor->enqueue_message(0));
    REQUIRE(actor->enqueue_message(1));

    actor->start(unthreaded);
    REQUIRE(actor->enqueue_message(2));
    pump_until_idle(*actor);
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == 3);
    for (int i = 0; i < 3; ++i)
        CHECK(impl->log[i] == i);
}

TEST("threaded_actor - unthreaded multi-type preserves global ordering")
{
    auto actor = cc::make_threaded_actor<multi_type_actor>();
    actor->start(unthreaded);

    REQUIRE(actor->enqueue_message(msg_a{1}));
    REQUIRE(actor->enqueue_message(msg_b{10}));
    REQUIRE(actor->enqueue_message(msg_a{2}));
    REQUIRE(actor->enqueue_message(msg_b{11}));
    REQUIRE(actor->enqueue_message(msg_b{12}));
    REQUIRE(actor->enqueue_message(msg_a{3}));
    pump_until_idle(*actor);
    actor->shutdown();

    auto impl = actor->take_impl<multi_type_actor>();
    REQUIRE(impl->log.size() == 6);
    char const types[] = {'A', 'B', 'A', 'B', 'B', 'A'};
    int const ids[] = {1, 10, 2, 11, 12, 3};
    for (int i = 0; i < 6; ++i)
    {
        CHECK(impl->log[i].type == types[i]);
        CHECK(impl->log[i].id == ids[i]);
    }
}

TEST("threaded_actor - unthreaded three message types")
{
    auto actor = cc::make_threaded_actor<three_type_actor>();
    actor->start(unthreaded);

    REQUIRE(actor->enqueue_message(msg_a{1}));
    REQUIRE(actor->enqueue_message(msg_b{2}));
    REQUIRE(actor->enqueue_message(msg_c{3}));
    REQUIRE(actor->enqueue_message(msg_a{4}));
    REQUIRE(actor->enqueue_message(msg_c{5}));
    REQUIRE(actor->enqueue_message(msg_b{6}));
    pump_until_idle(*actor);
    actor->shutdown();

    auto impl = actor->take_impl<three_type_actor>();
    REQUIRE(impl->tags.size() == 6);
    char const expected[] = {'A', 'B', 'C', 'A', 'C', 'B'};
    for (int i = 0; i < 6; ++i)
        CHECK(impl->tags[i] == expected[i]);
}

TEST("threaded_actor - unthreaded init runs at start, shutdown runs at shutdown")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();
    actor->start(unthreaded);

    // on_thread_init runs on the caller during start()
    {
        // peek without taking: not shut down yet, so use a message round-trip to observe liveness
        REQUIRE(actor->enqueue_message(1));
        pump_until_idle(*actor);
    }
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    CHECK(impl->init_count == 1);
    CHECK(impl->shutdown_count == 1);
    REQUIRE(impl->log.size() == 1);
}

TEST("threaded_actor - unthreaded init precedes messages, shutdown follows")
{
    auto actor = cc::make_threaded_actor<lifecycle_actor>();
    actor->start(unthreaded);

    for (int i = 0; i < 3; ++i)
        REQUIRE(actor->enqueue_message(i));
    pump_until_idle(*actor);
    actor->shutdown();

    auto impl = actor->take_impl<lifecycle_actor>();
    REQUIRE(impl->log.size() == 5);
    CHECK(impl->log.front() == lifecycle_actor::marker_init);
    CHECK(impl->log.back() == lifecycle_actor::marker_shutdown);
    for (int i = 0; i < 3; ++i)
        CHECK(impl->log[i + 1] == i);
}

TEST("threaded_actor - unthreaded on_process batches a local queue")
{
    auto output = std::make_shared<cc::vector<int>>();
    auto actor = cc::make_threaded_actor<batching_actor>(output);
    actor->start(unthreaded);

    for (int i = 1; i <= 3; ++i)
        REQUIRE(actor->enqueue_message(i));
    pump_until_idle(*actor);
    actor->shutdown();

    REQUIRE(output->size() == 3);
    for (int i = 0; i < 3; ++i)
        CHECK((*output)[i] == i + 1);

    auto impl = actor->take_impl<batching_actor>();
    CHECK(impl->process_calls > 0);
}

TEST("threaded_actor - unthreaded begin_shutdown rejects new messages but keeps queued ones")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();
    actor->start(unthreaded);

    for (int i = 0; i < 3; ++i)
        REQUIRE(actor->enqueue_message(i));

    actor->begin_shutdown();
    CHECK(!actor->enqueue_message(99));
    CHECK(!actor->enqueue_message(100));

    actor->shutdown(); // drains the 3 queued messages

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == 3);
    for (int i = 0; i < 3; ++i)
        CHECK(impl->log[i] == i);
}

TEST("threaded_actor - unthreaded shutdown drains everything without an explicit pump")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();

    constexpr int n = 50;
    for (int i = 0; i < n; ++i)
        REQUIRE(actor->enqueue_message(i));

    actor->start(unthreaded);
    actor->shutdown(); // final synchronous drain processes all queued messages

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == n);
    for (int i = 0; i < n; ++i)
        CHECK(impl->log[i] == i);
}

TEST("threaded_actor - unthreaded lifecycle state queries")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();

    CHECK(!actor->is_shutting_down());
    actor->start(unthreaded);
    CHECK(actor->is_running());
    CHECK(!actor->is_shutting_down());

    actor->begin_shutdown();
    CHECK(actor->is_shutting_down());
    CHECK(!actor->is_running());

    actor->shutdown();
    CHECK(actor->is_shut_down());
}

TEST("threaded_actor - process_messages_if_unthreaded reports more-to-do then goes idle")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();
    actor->start(unthreaded);

    REQUIRE(actor->enqueue_message(1));
    REQUIRE(actor->enqueue_message(2));
    CHECK(actor->process_messages_if_unthreaded());  // dispatched something
    CHECK(!actor->process_messages_if_unthreaded()); // nothing left

    actor->shutdown();
    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == 2);
}

TEST("threaded_actor - process_messages_if_unthreaded_for_ms drains within budget")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();
    actor->start(unthreaded);

    for (int i = 0; i < 10; ++i)
        REQUIRE(actor->enqueue_message(i));

    CHECK(!actor->process_messages_if_unthreaded_for_ms(1000.0)); // returns false: went idle
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == 10);
}

TEST("threaded_actor - unthreaded pipeline A -> B -> C forwarding")
{
    auto final_log = std::make_shared<cc::vector<cc::string>>();

    auto c = cc::make_threaded_actor<string_log_actor>(final_log);
    auto b = cc::make_threaded_actor<int_to_string_actor>(c.get());
    auto a = cc::make_threaded_actor<forwarding_actor>(b.get());

    a->start(unthreaded);
    b->start(unthreaded);
    c->start(unthreaded);

    constexpr int n = 10;
    for (int i = 0; i < n; ++i)
        REQUIRE(a->enqueue_message(i));

    // Pump in dependency order: A forwards to B, B to C.
    pump_until_idle(*a);
    pump_until_idle(*b);
    pump_until_idle(*c);

    a->shutdown();
    b->shutdown();
    c->shutdown();

    auto impl_a = a->take_impl<forwarding_actor>();
    REQUIRE(impl_a->log.size() == n);
    for (int i = 0; i < n; ++i)
        CHECK(impl_a->log[i] == i);

    REQUIRE(final_log->size() == n);
    for (int i = 0; i < n; ++i)
        CHECK((*final_log)[i] == cc::to_string(i));
}

// ============================================================================
// Threaded smoke tests (real background thread)
// ============================================================================

#if CC_HAS_THREADS

#include <latch>

TEST("threaded_actor - threaded start/shutdown with in-order delivery")
{
    auto actor = cc::make_and_start_threaded_actor<int_log_actor>();
    REQUIRE(actor->is_running());

    for (int i = 0; i < 20; ++i)
        REQUIRE(actor->enqueue_message(i));
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == 20);
    for (int i = 0; i < 20; ++i)
        CHECK(impl->log[i] == i);
    CHECK(impl->init_count == 1);
    CHECK(impl->shutdown_count == 1);
}

// Signals a latch on shutdown, so we can observe destructor-driven shutdown completing.
class raii_actor : public cc::threaded_actor_impl<int>
{
public:
    std::shared_ptr<cc::vector<int>> log;
    std::shared_ptr<std::latch> done;

    raii_actor(std::shared_ptr<cc::vector<int>> l, std::shared_ptr<std::latch> latch)
      : log(std::move(l)), done(std::move(latch))
    {
    }

protected:
    void on_message(int msg) override { log->push_back(msg); }
    void on_thread_shutdown() override { done->count_down(); }
};

TEST("threaded_actor - destructor triggers graceful shutdown")
{
    auto log = std::make_shared<cc::vector<int>>();
    auto done = std::make_shared<std::latch>(1);

    {
        auto actor = cc::make_and_start_threaded_actor<raii_actor>(log, done);
        REQUIRE(actor->enqueue_message(10));
        REQUIRE(actor->enqueue_message(20));
        REQUIRE(actor->enqueue_message(30));
        // handle destroyed here -> shutdown() joins the thread
    }

    done->wait();
    REQUIRE(log->size() == 3);
    CHECK((*log)[0] == 10);
    CHECK((*log)[1] == 20);
    CHECK((*log)[2] == 30);
}

TEST("threaded_actor - process_messages_if_unthreaded is a no-op in threaded mode")
{
    auto actor = cc::make_and_start_threaded_actor<int_log_actor>();

    CHECK(!actor->process_messages_if_unthreaded());
    CHECK(!actor->process_messages_if_unthreaded_for_ms(5.0));

    REQUIRE(actor->enqueue_message(1));
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == 1);
    CHECK(impl->log[0] == 1);
}

#endif // CC_HAS_THREADS
