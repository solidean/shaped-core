#include <clean-core/thread/async_thread_pool.hh>

#if CC_HAS_THREADS

#include <clean-core/string/print.hh>
#include <clean-core/thread/spin.hh>
#include <clean-core/thread/thread.hh>

// Work-stealing pool over the async_scheduler seam. See async_thread_pool.hh for the model; async_node.hh for
// the node state machine that makes concurrent poll()/schedule() safe.
//
// ============================================================================
// The wake protocol
// ============================================================================
//
// The requirement it exists to meet: no MPMC contention on the hot path when nobody is actively stealing. Note
// what that is really about -- COHERENCE TRAFFIC, not instruction count:
//
//   * fence(seq_cst) drains THIS core's store buffer. It touches no memory, transfers no cache line, and
//     invalidates nothing anywhere else. N cores fencing at once cost O(1) each. A local cost.
//   * loading _sleepers reads a line that, while nobody sleeps, is read-only for every core and sits Shared in
//     each L1. An L1 hit, no traffic.
//   * an RMW on a shared counter (`lock xadd`) needs the line EXCLUSIVE: it invalidates every other core's copy,
//     so the next core to push must pull it back. N cores pushing serialize on one line and it ping-pongs. O(N).
//
// So the steady-state cost of a push, on shared state, is zero atomic RMWs and zero coherence traffic. The only
// line the producer writes is its own deque's _bottom, which thieves read only while actively stealing -- which
// is exactly the carve-out the requirement grants. (This replaced a `_pending` counter that took a seq_cst RMW
// on every push AND every claim. It was also redundant: a worker's scan of the deques already answers "is there
// work" authoritatively, so it was a hot-path counter serving a cold-path question.)
//
// Correctness -- a Dekker store-load cross-pairing, and both sides must pay:
//
//   producer (hot):                          would-be sleeper (cold):
//     deque.push(node)                         _sleepers.fetch_add(1, seq_cst)   // RMW == its own full barrier
//       ... ends in a RELAXED _bottom store    fence(seq_cst)
//     fence(seq_cst)                <------->  re-scan every deque (loads _bottom)
//     if (_sleepers == 0) return;              if (found) { unregister; run it; }
//     ... bump _wake_epoch, notify ...         else wait on _wait_cv
//
// The producer's push ends in a relaxed store, and a relaxed store may be reordered past a later load (on x86 it
// simply sits in the store buffer). So "push, read _sleepers, skip if 0" is NOT safe on its own: the producer
// could read a stale _sleepers == 0 from before the sleeper registered, while the sleeper reads a stale _bottom
// from before the push -- both see nothing, the worker sleeps, and the task sits there forever.
//
// Sequential consistency gives one total order over the two fences and the sleeper's seq_cst RMW. Either the
// producer's _bottom store precedes the sleeper's scan in that order (the sleeper sees the work and does not
// sleep), or the sleeper's registration precedes the producer's load (the producer sees _sleepers > 0 and
// notifies). At least one always holds -- no lost wakeup. The _sleepers load itself can be relaxed: the fence
// supplies the ordering, and saying `relaxed` states what is actually required.
//
// The producer must still pass through _wait_m on the wake path, and an epoch counter does NOT remove that. The
// window: the sleeper holds _wait_m, evaluates the predicate (false), and is about to call wait(); the producer
// bumps the epoch and notifies nobody, because no waiter is registered yet; the sleeper then enqueues and sleeps
// forever. Only wait()'s atomic release-and-enqueue w.r.t. the mutex closes it. That cost lands on the wake path
// only, where a mutex acquire is noise next to the condvar round-trip it accompanies.
//
// _wake_epoch replaces `_pending > 0` as the condvar predicate: it is monotonic, so a wake cannot be missed or
// mistaken for a stale one, and it is touched ONLY when a sleeper exists -- never on the hot path.

thread_local cc::async_thread_pool::worker* cc::async_thread_pool::s_current_worker = nullptr;

// A 1-worker pool has nobody to steal from it, so it reports no steal-capable peers and the poll loop stops
// publishing deps it is about to drive inline anyway.
cc::async_thread_pool::async_thread_pool(int worker_count) : async_scheduler(worker_count > 1)
{
    CC_ASSERT(worker_count >= 1, "a thread pool needs at least one worker");

    _workers.reserve(worker_count);
    for (int i = 0; i < worker_count; ++i)
    {
        auto w = cc::make_unique<worker>();
        w->pool = this;
        w->id = i;
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

    // Every worker is joined, so two things are now true and both are load-bearing:
    //
    //  * The deques hold RAW pointers, each owning a strong count nobody will ever claim. Unlike a queue of
    //    handles, they do not free themselves -- abandoned work would pin its entire graph forever. So drain
    //    them by hand: adopting and immediately dropping each entry pays back the count push_local handed over.
    //  * try_take is owner-only, but "owner" means "the one thread touching this deque", and with the workers
    //    gone that is us. Same reason ~chase_lev_deque may free its retired buffers: no thief can exist.
    //
    // The injection queue needs none of this -- it holds real handles, so its vector frees them itself.
    for (auto& w : _workers)
    {
        async_node_base* raw = nullptr;
        while (w->deque.try_take(raw))
            (void)async_node_ptr::adopt(raw); // adopt + immediate drop == release the count
    }
}

void cc::async_thread_pool::enqueue(async_node_ptr node)
{
    worker* w = s_current_worker;
    CC_ASSERT(w != nullptr && w->pool == this, "enqueue() must be called from a worker of this pool");
    push_local(*w, cc::move(node));
}

void cc::async_thread_pool::submit(async_node_ptr node)
{
    CC_ASSERT(node != nullptr, "cannot submit a null node");

    // The injection queue stays a plain mutex, deliberately: only genuinely foreign threads reach submit() (a
    // worker waking a node routes through enqueue via async_scheduler::current_or_null), so this is cold by
    // construction and a lock-free rewrite would buy nothing.
    _injection.lock([&](cc::vector<async_node_ptr>& q) { q.push_back(cc::move(node)); });
    wake_one();
}

void cc::async_thread_pool::push_local(worker& w, async_node_ptr node)
{
    // Hand the strong count to the deque: release() gives up ownership without dropping the count, so this is
    // count-neutral -- no inc/dec pair for the round trip. From here the deque owes the release, either to
    // whoever claims the entry or to the pool destructor's drain.
    w.deque.push(node.release());
    wake_one();
}

void cc::async_thread_pool::wake_one()
{
    // Our half of the Dekker (see the protocol block): the push above ended in a relaxed store, so without this
    // fence the load below could be satisfied from before a sleeper registered -- and that sleeper's own scan
    // could be satisfied from before our push. Both would see nothing and the task would strand.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    if (_sleepers.load(std::memory_order_relaxed) == 0)
        return; // the steady state: nobody asleep, so no traffic beyond an L1 read of a Shared line

    // A sleeper exists (or is committing to sleep). Bump the epoch under _wait_m: taking the lock is what closes
    // the check-then-wait window, and the epoch is what makes the predicate immune to a missed notify.
    {
        std::lock_guard<std::mutex> const lk(_wait_m);
        _wake_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    _wait_cv.notify_one();
}

cc::async_node_ptr cc::async_thread_pool::try_get_work(worker& w)
{
    // 1. our own deque, LIFO (hot: freshly spawned children). Uncontended in the common case.
    async_node_base* raw = nullptr;
    if (w.deque.try_take(raw))
        return async_node_ptr::adopt(raw); // take the strong count back out of the deque

    // 2. steal from a sibling's opposite (old) end -- the oldest entry is the coldest and usually roots the
    //    biggest subtree.
    //
    //    Victims are picked at RANDOM, not scanned in order: a linear scan from worker 0 points every idle
    //    worker at the same victim, which both concentrates CAS contention on one deque and starves the rest.
    //    Bounded at 2n attempts so a worker with genuinely nothing to steal reaches the sleep path instead of
    //    spinning here forever. An `abort` costs an attempt and moves on -- retrying a different victim is the
    //    better move, and whoever beat us to that node is running it.
    int const n = int(_workers.size());
    if (n > 1)
    {
        int const attempts = 2 * n;
        for (int i = 0; i < attempts; ++i)
        {
            int const victim = int(w.rng.next_u32() % cc::u32(n));
            if (victim == w.id)
                continue;

            if (_workers[victim]->deque.try_steal(raw) == cc::impl::steal_result::success)
                return async_node_ptr::adopt(raw);
        }
    }

    // 3. the shared injection queue (foreign submits). Strong handles: it is cold, so it stays a plain mutex.
    if (auto n = _injection.lock(
            [](cc::vector<async_node_ptr>& q) -> async_node_ptr
            {
                if (q.empty())
                    return nullptr;
                return q.pop_back();
            }))
        return n;

    return nullptr;
}

// How many times a worker re-scans for work before committing to the condvar. Not a micro-optimization: a
// condvar round-trip is ~1-10 us, while a spinning worker picks it up in ~100 ns, and fork-join graphs run tasks
// that cost ~100 ns each and go briefly dry all the time -- sleeping instantly pays microseconds to save
// nanoseconds.
//
// Measured, not guessed (spawn tree at 20 workers, i9-12900H, ns/node): 0 -> 18.2, 16 -> 14.2, 64 -> 13.6,
// 256 -> 14.0. So spinning at all is worth ~25%, and past ~64 the workers just burn cores that the graph itself
// wants (256 was clearly worse on parallel-for). 16 vs 64 is close; 64 won consistently on repeat.
static constexpr int async_pool_spin_rounds = 64;

void cc::async_thread_pool::worker_main(worker& w)
{
    cc::set_current_thread_name("async-pool");
    s_current_worker = &w;
    async_worker_scope const scope(*this);

    while (!_stop.load(std::memory_order_acquire))
    {
        if (auto n = try_get_work(w))
        {
            n->poll();
            continue;
        }

        // 1. nothing right now -- spin a bounded while before paying for a sleep. No shared writes here, just
        //    re-scans, so a worker that is about to be handed work costs nobody anything.
        bool found = false;
        for (int i = 0; i < async_pool_spin_rounds && !_stop.load(std::memory_order_relaxed); ++i)
        {
            cc::spin_pause();
            if (auto n = try_get_work(w))
            {
                n->poll();
                found = true;
                break;
            }
        }
        if (found)
            continue;

        // 2. still nothing: commit to sleeping. Capture the epoch BEFORE registering, so a wake that lands
        //    between here and the wait cannot be mistaken for one we already consumed.
        cc::i64 const epoch = _wake_epoch.load(std::memory_order_acquire);

        // Our half of the Dekker (see the protocol block). The RMW is a full barrier in its own right; the fence
        // spells out what it is for and keeps the pairing legible if the RMW is ever weakened.
        _sleepers.fetch_add(1, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // 3. the re-scan that closes the race: a producer that pushed before our registration became visible may
        //    have already read _sleepers == 0 and skipped notifying us. Seq_cst says at least one of us sees the
        //    other -- so if they missed us, we see their work here.
        if (auto n = try_get_work(w))
        {
            _sleepers.fetch_sub(1, std::memory_order_relaxed);
            n->poll();
            continue;
        }

        {
            std::unique_lock<std::mutex> lk(_wait_m);
            _wait_cv.wait(
                lk, [&]
                { return _stop.load(std::memory_order_relaxed) || _wake_epoch.load(std::memory_order_relaxed) != epoch; });
        }
        _sleepers.fetch_sub(1, std::memory_order_relaxed);
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
