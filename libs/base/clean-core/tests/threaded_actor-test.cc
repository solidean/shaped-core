#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/to_string.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <nexus/test.hh>

#include <atomic>
#include <latch>
#include <memory>

using namespace cc::primitive_defines;

// ============================================================================
// Test actors
// ============================================================================

// Single-type actor logging int messages in order.
class int_log_actor : public cc::threaded_actor_impl<int>
{
public:
    cc::vector<int> log;
    std::atomic<int> init_count{0};
    std::atomic<int> shutdown_count{0};

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

// Batches messages into a local queue and doubles them out in on_process.
class batching_actor : public cc::threaded_actor_impl<int>
{
public:
    cc::vector<int> local_queue;
    std::shared_ptr<cc::vector<int>> output;
    std::atomic<int> process_calls{0};

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

// Signals a latch on shutdown, for RAII tests.
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

// ============================================================================
// Lifecycle and ordering
// ============================================================================

TEST("threaded_actor - single-type start/shutdown with in-order delivery")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();

    actor->start();
    for (int i = 0; i < 5; ++i)
        REQUIRE(actor->enqueue_message(i));
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl != nullptr);
    REQUIRE(impl->log.size() == 5);
    for (int i = 0; i < 5; ++i)
        CHECK(impl->log[i] == i);
}

TEST("threaded_actor - pre-start messages are queued and processed after start")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();

    REQUIRE(actor->enqueue_message(0));
    REQUIRE(actor->enqueue_message(1));
    REQUIRE(actor->enqueue_message(2));

    actor->start();
    REQUIRE(actor->enqueue_message(3));
    REQUIRE(actor->enqueue_message(4));
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == 5);
    for (int i = 0; i < 5; ++i)
        CHECK(impl->log[i] == i);
}

TEST("threaded_actor - multi-type preserves global ordering across types")
{
    auto actor = cc::make_threaded_actor<multi_type_actor>();

    actor->start();
    REQUIRE(actor->enqueue_message(msg_a{1}));
    REQUIRE(actor->enqueue_message(msg_b{10}));
    REQUIRE(actor->enqueue_message(msg_a{2}));
    REQUIRE(actor->enqueue_message(msg_b{11}));
    REQUIRE(actor->enqueue_message(msg_b{12}));
    REQUIRE(actor->enqueue_message(msg_a{3}));
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

TEST("threaded_actor - three message types")
{
    auto actor = cc::make_threaded_actor<three_type_actor>();

    actor->start();
    REQUIRE(actor->enqueue_message(msg_a{1}));
    REQUIRE(actor->enqueue_message(msg_b{2}));
    REQUIRE(actor->enqueue_message(msg_c{3}));
    REQUIRE(actor->enqueue_message(msg_a{4}));
    REQUIRE(actor->enqueue_message(msg_c{5}));
    REQUIRE(actor->enqueue_message(msg_b{6}));
    actor->shutdown();

    auto impl = actor->take_impl<three_type_actor>();
    REQUIRE(impl->tags.size() == 6);
    char const expected[] = {'A', 'B', 'C', 'A', 'C', 'B'};
    for (int i = 0; i < 6; ++i)
        CHECK(impl->tags[i] == expected[i]);
}

// ============================================================================
// Thread init/shutdown hooks
// ============================================================================

TEST("threaded_actor - on_thread_init and on_thread_shutdown run exactly once")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();

    actor->start();
    REQUIRE(actor->enqueue_message(1));
    REQUIRE(actor->enqueue_message(2));
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    CHECK(impl->init_count == 1);
    CHECK(impl->shutdown_count == 1);
}

TEST("threaded_actor - init precedes messages and shutdown follows them")
{
    auto actor = cc::make_threaded_actor<lifecycle_actor>();

    actor->start();
    for (int i = 0; i < 3; ++i)
        REQUIRE(actor->enqueue_message(i));
    actor->shutdown();

    auto impl = actor->take_impl<lifecycle_actor>();
    REQUIRE(impl->log.size() >= 2);
    CHECK(impl->log.front() == lifecycle_actor::marker_init);
    CHECK(impl->log.back() == lifecycle_actor::marker_shutdown);

    int init_count = 0;
    int shutdown_count = 0;
    for (int v : impl->log)
    {
        if (v == lifecycle_actor::marker_init)
            init_count++;
        if (v == lifecycle_actor::marker_shutdown)
            shutdown_count++;
    }
    CHECK(init_count == 1);
    CHECK(shutdown_count == 1);
}

TEST("threaded_actor - on_process batches a local queue")
{
    auto output = std::make_shared<cc::vector<int>>();
    auto actor = cc::make_threaded_actor<batching_actor>(output);

    actor->start();
    REQUIRE(actor->enqueue_message(1));
    REQUIRE(actor->enqueue_message(2));
    REQUIRE(actor->enqueue_message(3));
    actor->shutdown();

    REQUIRE(output->size() == 3);
    for (int i = 0; i < 3; ++i)
        CHECK((*output)[i] == i + 1);

    auto impl = actor->take_impl<batching_actor>();
    CHECK(impl->process_calls > 0);
}

// ============================================================================
// Shutdown semantics
// ============================================================================

TEST("threaded_actor - begin_shutdown rejects new messages but keeps queued ones")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();

    actor->start();
    REQUIRE(actor->enqueue_message(0));
    REQUIRE(actor->enqueue_message(1));
    REQUIRE(actor->enqueue_message(2));

    actor->begin_shutdown();
    CHECK(!actor->enqueue_message(99));
    CHECK(!actor->enqueue_message(100));

    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == 3);
    for (int i = 0; i < 3; ++i)
        CHECK(impl->log[i] == i);
}

TEST("threaded_actor - shutdown drains all messages sent before it begins")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();

    constexpr int n = 100;
    for (int i = 0; i < n; ++i)
        REQUIRE(actor->enqueue_message(i));

    actor->start();
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == n);
    for (int i = 0; i < n; ++i)
        CHECK(impl->log[i] == i);
}

TEST("threaded_actor - is_shutting_down reflects lifecycle state")
{
    auto actor = cc::make_threaded_actor<int_log_actor>();

    CHECK(!actor->is_shutting_down());
    actor->start();
    CHECK(!actor->is_shutting_down());
    CHECK(actor->is_running());

    actor->begin_shutdown();
    CHECK(actor->is_shutting_down());
    CHECK(!actor->is_running());

    actor->shutdown();
    CHECK(actor->is_shutting_down());
    CHECK(actor->is_shut_down());
}

TEST("threaded_actor - destructor triggers graceful shutdown")
{
    auto log = std::make_shared<cc::vector<int>>();
    auto done = std::make_shared<std::latch>(1);

    {
        auto actor = cc::make_threaded_actor<raii_actor>(log, done);
        actor->start();
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

// ============================================================================
// Pipelines
// ============================================================================

TEST("threaded_actor - three-actor pipeline A -> B -> C forwarding")
{
    auto final_log = std::make_shared<cc::vector<cc::string>>();

    // build downstream-first so upstream pointers are valid
    auto c = cc::make_threaded_actor<string_log_actor>(final_log);
    auto b = cc::make_threaded_actor<int_to_string_actor>(c.get());
    auto a = cc::make_threaded_actor<forwarding_actor>(b.get());

    a->start();
    b->start();
    c->start();

    constexpr int n = 10;
    for (int i = 0; i < n; ++i)
        REQUIRE(a->enqueue_message(i));

    // sequential shutdown drains the pipeline: each shutdown() blocks until that stage has
    // forwarded everything, so the next stage is still accepting when it does.
    a->shutdown();
    b->shutdown();
    c->shutdown();

    auto impl_a = a->take_impl<forwarding_actor>();
    REQUIRE(impl_a->log.size() == n);
    for (int i = 0; i < n; ++i)
        CHECK(impl_a->log[i] == i);

    auto impl_b = b->take_impl<int_to_string_actor>();
    REQUIRE(impl_b->log.size() == n);

    REQUIRE(final_log->size() == n);
    for (int i = 0; i < n; ++i)
        CHECK((*final_log)[i] == cc::to_string(i));
}

TEST("threaded_actor - make_and_start_threaded_actor starts immediately")
{
    auto actor = cc::make_and_start_threaded_actor<int_log_actor>();

    REQUIRE(actor->is_running());
    REQUIRE(actor->enqueue_message(7));
    actor->shutdown();

    auto impl = actor->take_impl<int_log_actor>();
    REQUIRE(impl->log.size() == 1);
    CHECK(impl->log[0] == 7);
}
