#include <clean-core/thread/async_thread_pool.hh>

#if CC_HAS_THREADS

#include <clean-core/string/print.hh>
#include <clean-core/thread/thread.hh>

// Work-stealing pool over the async_scheduler seam. See async_thread_pool.hh for the model; async_node.hh for
// the node state machine that makes concurrent poll()/schedule() safe.
//
// Wakeup handshake (no lost wakeups, no hot-path mutex when nobody sleeps):
//   * _pending counts claimable tasks across every deque + the injection queue (+1 on push, -1 on claim).
//   * a worker that finds no work does _sleepers++ (seq_cst) then waits on _wait_cv with a predicate that
//     re-reads _pending under _wait_m;
//   * a producer does _pending++ (seq_cst) then, only if _sleepers > 0, takes _wait_m briefly and notifies.
//   The seq_cst store/load cross-pairs (_pending vs _sleepers) guarantee at least one side observes the other,
//   so a task pushed just as a worker decides to sleep is never missed.

thread_local cc::async_thread_pool::worker* cc::async_thread_pool::s_current_worker = nullptr;

cc::async_thread_pool::async_thread_pool(int worker_count, async_affinity served) : _served(served)
{
    CC_ASSERT(worker_count >= 1, "a thread pool needs at least one worker");
    CC_ASSERT(!served.is_empty(), "a pool must serve at least one affinity class");

    _workers.reserve(worker_count);
    for (int i = 0; i < worker_count; ++i)
    {
        auto w = cc::make_unique<worker>();
        w->pool = this;
        w->id = i;
        w->served = served;
        _workers.push_back(cc::move(w));
    }

    // start threads only after every worker slot exists, so a stealer always sees all deques
    for (auto& w : _workers)
        w->thread = std::thread([this, wp = w.get()] { worker_main(*wp); });
}

cc::async_thread_pool::~async_thread_pool()
{
    CC_ASSERT(async_scheduler::default_or_null() != static_cast<async_scheduler*>(this),
              "uninstall this pool as the default before destroying it (uninstall_default_async_pool / "
              "scoped_default_async_pool)");

    _stop.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(_wait_m); // synchronize with any worker between predicate-check and wait
    }
    _wait_cv.notify_all();

    for (auto& w : _workers)
        if (w->thread.joinable())
            w->thread.join();
}

void cc::async_thread_pool::enqueue(std::shared_ptr<async_node_base> node)
{
    worker* w = s_current_worker;
    CC_ASSERT(w != nullptr && w->pool == this, "enqueue() must be called from a worker of this pool");
    push_local(*w, cc::move(node));
}

void cc::async_thread_pool::submit(std::shared_ptr<async_node_base> node)
{
    CC_ASSERT(node != nullptr, "cannot submit a null node");
    CC_ASSERT(node->affinity().overlaps(_served), "submitted a node whose affinity this pool does not serve");

    _injection.lock([&](cc::vector<std::shared_ptr<async_node_base>>& q) { q.push_back(cc::move(node)); });
    _pending.fetch_add(1, std::memory_order_seq_cst);
    wake_one();
}

void cc::async_thread_pool::push_local(worker& w, std::shared_ptr<async_node_base> node)
{
    w.deque.lock([&](cc::vector<std::shared_ptr<async_node_base>>& q) { q.push_back(cc::move(node)); });
    _pending.fetch_add(1, std::memory_order_seq_cst);
    wake_one();
}

void cc::async_thread_pool::wake_one()
{
    if (_sleepers.load(std::memory_order_seq_cst) == 0)
        return; // fast path: nobody is asleep, so nothing to wake

    {
        std::lock_guard<std::mutex> lk(_wait_m); // ensure we don't slip into a worker's check-then-wait window
    }
    _wait_cv.notify_one();
}

std::shared_ptr<cc::async_node_base> cc::async_thread_pool::try_get_work(worker& w)
{
    // 1. our own deque, LIFO (hot: freshly spawned children)
    if (auto n = w.deque.lock(
            [](cc::vector<std::shared_ptr<async_node_base>>& q) -> std::shared_ptr<async_node_base>
            {
                if (q.empty())
                    return nullptr;
                return q.pop_back();
            }))
        return n;

    // 2. steal from a sibling's opposite (old) end, taking the first affinity-compatible task
    for (auto& other : _workers)
    {
        if (other.get() == &w)
            continue;

        auto stolen = other->deque.try_lock(
            [&](cc::vector<std::shared_ptr<async_node_base>>& q) -> std::shared_ptr<async_node_base>
            {
                for (cc::isize i = 0; i < q.size(); ++i)
                    if (q[i]->affinity().overlaps(w.served))
                        return q.pop_at(i);
                return nullptr;
            });
        if (stolen.has_value() && stolen.value() != nullptr)
            return stolen.value();
    }

    // 3. the shared injection queue (foreign submits / cross-affinity wakeups)
    if (auto n = _injection.lock(
            [](cc::vector<std::shared_ptr<async_node_base>>& q) -> std::shared_ptr<async_node_base>
            {
                if (q.empty())
                    return nullptr;
                return q.pop_back();
            }))
        return n;

    return nullptr;
}

void cc::async_thread_pool::worker_main(worker& w)
{
    cc::set_current_thread_name("async-pool");
    s_current_worker = &w;
    async_worker_scope scope(*this, w.served);

    while (!_stop.load(std::memory_order_acquire))
    {
        if (auto n = try_get_work(w))
        {
            _pending.fetch_sub(1, std::memory_order_seq_cst);
            n->poll();
            continue;
        }

        // no work found: register as a sleeper and wait until a producer signals or we are stopped. The
        // predicate re-reads _pending under _wait_m, so work pushed during this window is not missed.
        _sleepers.fetch_add(1, std::memory_order_seq_cst);
        {
            std::unique_lock<std::mutex> lk(_wait_m);
            _wait_cv.wait(
                lk,
                [&] { return _stop.load(std::memory_order_seq_cst) || _pending.load(std::memory_order_seq_cst) > 0; });
        }
        _sleepers.fetch_sub(1, std::memory_order_seq_cst);
    }

    s_current_worker = nullptr;
}

void cc::async_thread_pool::block_until_ready(async_node_base& root)
{
    struct sync
    {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
    };
    sync s;

    // notify UNDER the lock so this hook (running on a worker) fully returns before block_until_ready's frame
    // (and thus `s`) is destroyed.
    bool const already = root.install_completion_hook_or_ready(
        [](void* p)
        {
            auto* sp = static_cast<sync*>(p);
            std::lock_guard<std::mutex> lk(sp->m);
            sp->done = true;
            sp->cv.notify_one();
        },
        &s);

    root.schedule_on(*this); // force the root onto this pool (works whether or not it is the installed default)

    if (already)
        return; // completed before we installed the hook: no wait, no notify pending

    std::unique_lock<std::mutex> lk(s.m);
    s.cv.wait(lk, [&] { return s.done; });
}

void cc::install_default_async_pool(async_thread_pool& pool)
{
    CC_ASSERT(async_scheduler::default_or_null() == nullptr,
              "a default async pool is already installed; overriding a live default is almost never correct "
              "(uninstall it first, or use scoped_default_async_pool)");
    async_scheduler::set_default(&pool);
}

void cc::uninstall_default_async_pool(async_thread_pool& pool)
{
    CC_ASSERT(async_scheduler::default_or_null() == static_cast<async_scheduler*>(&pool),
              "uninstall_default_async_pool: this pool is not the currently installed default");
    async_scheduler::set_default(nullptr);
}

#endif // CC_HAS_THREADS
