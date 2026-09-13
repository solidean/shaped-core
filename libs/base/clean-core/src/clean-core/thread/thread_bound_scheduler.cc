#include <clean-core/common/assert.hh>
#include <clean-core/common/time.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/impl/async_tls.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_bound_scheduler.hh>
#include <clean-core/thread/thread_pump.hh>

#include <chrono>
#if CC_HAS_THREADS
#include <thread>
#endif

using namespace cc::primitive_defines;

namespace
{
/// The main home, created by mark_current_thread_as_main() and never destroyed.
/// Immortal for the same reason the pump registry is: a node homed to main may be dropped during static destruction.
cc::atomic<cc::thread_bound_scheduler*> s_main_home = {nullptr};
} // namespace

namespace cc::impl
{
/// Called once from mark_current_thread_as_main(), on the main thread.
void async_bind_main_thread_home()
{
    CC_ASSERT(s_main_home.load(cc::memory_order_acquire) == nullptr, "the main thread's home is already bound");
    auto* const home = new cc::thread_bound_scheduler();
    home->bind_to_current_thread();
    s_main_home.store(home, cc::memory_order_release);
}
} // namespace cc::impl

cc::async_scheduler& cc::impl::async_main_home()
{
    return cc::main_thread_scheduler();
}

cc::thread_bound_scheduler& cc::main_thread_scheduler()
{
    auto* const home = s_main_home.load(cc::memory_order_acquire);
    CC_ASSERT(home != nullptr, "no main thread home: call cc::mark_current_thread_as_main() from main() first");
    return *home;
}

cc::thread_bound_scheduler::thread_bound_scheduler(async_inline_deps default_inline_deps)
  : async_scheduler(true, default_inline_deps)
{
    // Steal-capable on purpose: a homed driver whose policy allows inline driving still publishes its other
    // dependencies, and this home's enqueue forwards those to compute rather than keeping them.
}

cc::thread_bound_scheduler::~thread_bound_scheduler()
{
    CC_ASSERT(!is_bound() || is_owner_thread(), "a thread_bound_scheduler must be destroyed on its owner thread");

    // Whatever is still queued is run or torn down here, in place: this IS the owner thread.
    // The base destructor then checks that nothing homed here outlived it.
    auto it = item();
    while (take_one(it))
        run(it);

    if (is_bound() && cc::impl::async_tls().home == this)
        cc::impl::async_tls().home = nullptr;
}

void cc::thread_bound_scheduler::bind_to_current_thread()
{
    CC_ASSERT(!is_bound(), "a thread_bound_scheduler is bound at most once");
    CC_ASSERT(cc::impl::async_tls().home == nullptr, "this thread already owns a home");
    _owner = cc::current_thread_id();
    cc::impl::async_tls().home = this;
}

bool cc::thread_bound_scheduler::is_owner_thread() const
{
    return _owner == cc::current_thread_id();
}

void cc::thread_bound_scheduler::enqueue(async_node_ptr node)
{
    // Only unhomed work arrives here: a homed node routes itself through submit.
    // So this is a body of ours scheduling a child or a dependency, and a home never keeps work it did not ask for.
    auto* const compute = async_scheduler::compute_or_null();
    CC_ASSERT(compute != nullptr && compute != this, "unhomed work scheduled from a thread home needs an installed "
                                                     "compute scheduler to run on");
    compute->submit(cc::move(node));
}

void cc::thread_bound_scheduler::submit(async_node_ptr node)
{
    CC_ASSERT(node != nullptr, "cannot submit a null node");
    push(item{.node = node.release(), .teardown = false});
}

bool cc::thread_bound_scheduler::try_defer_teardown(async_node_base* node)
{
    // On the owner, outside our own bodies, tearing down in place IS tearing down at home.
    if (is_owner_thread())
        return false;

    push(item{.node = node, .teardown = true});
    return true;
}

void cc::thread_bound_scheduler::push(item it)
{
#if CC_HAS_THREADS
    {
        std::lock_guard const lock(_mutex);
        _incoming.push_back(it);
        // Read under the same mutex the parked owner took for its last look at the queue.
        // Either that look saw this item, or the owner had already announced where it parks, and we wake it there.
        // The wake happens under the mutex too: the owner clears _parked_in under it before leaving the pool, so the pool
        // named here cannot be destroyed while we wake it.
        if (auto* const parked_in = _parked_in.load(cc::memory_order_acquire))
            parked_in->wake_home_participants();
    }
    _work_cv.notify_one();
#else
    _local.push_back(it);
#endif
}

bool cc::thread_bound_scheduler::take_one(item& out)
{
    if (_local_next == _local.size())
    {
        _local.clear();
        _local_next = 0;
#if CC_HAS_THREADS
        std::lock_guard const lock(_mutex);
        cc::swap_by_move(_local, _incoming);
#endif
        if (_local.empty())
            return false;
    }

    out = _local[_local_next++];
    return true;
}

void cc::thread_bound_scheduler::run(item it)
{
    if (it.teardown)
    {
        cc::impl::async_run_deferred_teardown(it.node);
        return;
    }

    struct body_scope
    {
        int& depth;
        explicit body_scope(int& d) : depth(d) { ++depth; }
        ~body_scope() { --depth; }
        body_scope(body_scope const&) = delete;
        body_scope& operator=(body_scope const&) = delete;
    };

    body_scope const in_body(_body_depth);
    auto const node
        = async_node_ptr::adopt(it.node); // dropped inside the body scope: a value's destructor is still one of our bodies
    async_worker_scope const scope(*this);
    impl::async_poll_work_item(*node);
}

bool cc::thread_bound_scheduler::try_run_one()
{
    if (!is_owner_thread() || _body_depth > 0)
        return false;

    auto it = item();
    if (!take_one(it))
        return false;

    run(it);
    return true;
}

bool cc::thread_bound_scheduler::has_queued_work() const
{
    if (_local_next != _local.size())
        return true;
#if CC_HAS_THREADS
    std::lock_guard const lock(_mutex);
    return !_incoming.empty();
#else
    return false;
#endif
}

bool cc::thread_bound_scheduler::pump_cycle(double deadline_secs)
{
    if (!is_owner_thread() || _body_depth > 0)
        return false;

    // Bounded by what is queued right now: a body that yields re-queues itself behind this snapshot, not in front of it.
    // The snapshot is moved out before anything runs, because a deferred teardown runs outside any body and may reach a
    // nested cycle through a blocking wait, which refills _local under our feet.
    auto batch = cc::vector<item>();
    if (_local_next == _local.size())
    {
        _local.clear();
        _local_next = 0;
#if CC_HAS_THREADS
        std::lock_guard const lock(_mutex);
        cc::swap_by_move(batch, _incoming);
#else
        cc::swap_by_move(batch, _local);
#endif
    }
    else
    {
        batch.push_back_range(cc::span<item const>(_local).subspan(_local_next));
        _local.clear();
        _local_next = 0;
    }

    if (batch.empty())
        return false;

    for (isize i = 0; i < batch.size(); ++i)
    {
        run(batch[i]);

        // Checked between items, so one long body overruns the budget; what is left goes back to the front of the queue.
        if (deadline_secs > 0 && i + 1 < batch.size() && cc::current_time_steady_secs() >= deadline_secs)
        {
            auto rest = cc::vector<item>();
            rest.push_back_range(cc::span<item const>(batch).subspan(i + 1));
            rest.push_back_range(cc::span<item const>(_local).subspan(_local_next));
            _local = cc::move(rest);
            _local_next = 0;
            return true;
        }
    }
    return true;
}

void cc::thread_bound_scheduler::drain()
{
    CC_ASSERT(is_owner_thread(), "drain runs a thread home's queue, which only its owner thread may do");
    CC_ASSERT(_body_depth == 0, "drain from inside one of this home's own bodies would re-enter it");
    while (pump_cycle())
    {
    }
}

bool cc::thread_bound_scheduler::pump_for(double max_ms)
{
    CC_ASSERT(is_owner_thread(), "pump_for runs a thread home's queue, which only its owner thread may do");

    if (max_ms <= 0)
    {
        (void)pump_cycle();
        return has_queued_work();
    }

    auto const deadline = cc::current_time_steady_secs() + max_ms / 1000.0;
    while (pump_cycle(deadline))
        if (cc::current_time_steady_secs() >= deadline)
            return has_queued_work();
    return false;
}

void cc::thread_bound_scheduler::participate_until_ready(async_node_base& root)
{
    auto* const compute = async_scheduler::compute_or_null();
    CC_ASSERT(compute != nullptr && compute != this, "a blocking wait inside a thread home needs an installed compute "
                                                     "scheduler to drive the graph on");
    compute->participate_until_ready(root);
}

void cc::thread_bound_scheduler::wait_for_work(double max_ms)
{
#if CC_HAS_THREADS
    // Inside one of our own bodies the queue is not ours to run, so queued work is no reason to stop waiting.
    if (_body_depth > 0 && is_owner_thread())
    {
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(max_ms));
        return;
    }

    std::unique_lock lock(_mutex);
    if (!_incoming.empty() || _local_next != _local.size())
        return;
    _work_cv.wait_for(lock, std::chrono::duration<double, std::milli>(max_ms));
#else
    CC_UNUSED(max_ms); // nothing else can submit while the one thread there is waits
#endif
}

bool cc::pump_main_thread(double max_ms)
{
    CC_ASSERT(cc::current_thread_id() == cc::thread_id::main, "cc::pump_main_thread must be called on the main thread");

    auto& home = main_thread_scheduler();

    auto const step_threadless = [](async_scheduler* s)
    {
        // A scheduler with no peers has nobody else to run its queue: the loop's pump is where it happens.
        if (s == nullptr || s->has_steal_capable_peers)
            return false;
        return s->try_run_one();
    };

    auto const cycle = [&](double deadline_secs)
    {
        auto more = home.pump_cycle(deadline_secs);
        more |= cc::impl::thread_pump_registry(); // not thread_pump_all: that would run the home again, past the budget
        auto* const compute = async_scheduler::compute_or_null();
        auto* const io = async_scheduler::io_or_null();
        more |= step_threadless(compute);
        if (io != compute)
            more |= step_threadless(io);
        return more;
    };

    // Only the home's queue can be asked whether work is left; the registry and a threadless scheduler report progress.
    // So "pending" is either: a loop that sees false has nothing to do but wait.
    if (max_ms <= 0)
        return cycle(0) || home.has_queued_work();

    auto const deadline = cc::current_time_steady_secs() + max_ms / 1000.0;
    while (cycle(deadline))
        if (cc::current_time_steady_secs() >= deadline)
            return true;
    return home.has_queued_work();
}
