#include <clean-core/thread/async_thread_pool.hh>

#if CC_HAS_THREADS
#include <clean-core/string/print.hh>
#include <clean-core/thread/spin.hh>
#include <clean-core/thread/thread.hh>
#endif

// Work-stealing pool over the async_scheduler seam. See async_thread_pool.hh for the model; async_node.hh for
// the node state machine that makes concurrent poll()/schedule() safe. Two compile-time frontends: everything
// down to the #else is the threaded one; the no-threads pool is a short block at the bottom.
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

#if CC_HAS_THREADS

thread_local cc::async_thread_pool::worker* cc::async_thread_pool::s_current_worker = nullptr;

int cc::async_thread_pool::default_worker_count()
{
    int const n = cc::num_hardware_threads() - 1; // the blocking_get caller runs work too; leave it a core
    return n < 1 ? 1 : n;
}

// Always steal-capable, even at worker_count == 1: the external slots mean a foreign blocking_get caller is a
// second participant that can steal from the worker and be stolen from, so the poll loop must keep publishing
// dependencies rather than assume it is alone and drive them all inline.
cc::async_thread_pool::async_thread_pool(int worker_count) : async_scheduler(true)
{
    CC_ASSERT(worker_count >= 1, "a thread pool needs at least one worker");
    _thread_count = worker_count;

    _workers.reserve(worker_count + external_slot_count);
    for (int i = 0; i < worker_count + external_slot_count; ++i)
    {
        auto w = cc::make_unique<worker>();
        w->pool = this;
        w->id = i;
        _workers.push_back(cc::move(w));
    }

    // start threads only after every slot exists (external ones included), so a stealer always sees all deques
    for (int i = 0; i < _thread_count; ++i)
        _workers[i]->thread = std::thread([this, wp = _workers[i].get()] { worker_main(*wp); });
}

cc::async_thread_pool::~async_thread_pool()
{
    CC_ASSERT(async_scheduler::default_or_null() != static_cast<async_scheduler*>(this),
              "uninstall this pool as the default before destroying it (uninstall_default_async_pool / "
              "scoped_default_async_pool)");

    _stop.store(true, cc::memory_order_release);
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
    // worker waking a node routes through enqueue via async_scheduler::current_or_null), so the PUSH side is
    // cold by construction and a lock-free rewrite would buy nothing. Its POP side is not cold — that is what
    // _injection_hint exists for; see the member.
    _injection.lock([&](cc::vector<async_node_ptr>& q) { q.push_back(cc::move(node)); });
    _injection_hint.fetch_add(1, cc::memory_order_relaxed);
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
    cc::atomic_thread_fence(cc::memory_order_seq_cst);

    if (_sleepers.load(cc::memory_order_relaxed) == 0)
        return; // the steady state: nobody asleep, so no traffic beyond an L1 read of a Shared line

    // A sleeper exists (or is committing to sleep). Bump the epoch under _wait_m: taking the lock is what closes
    // the check-then-wait window, and the epoch is what makes the predicate immune to a missed notify.
    {
        std::lock_guard<std::mutex> const lk(_wait_m);
        _wake_epoch.fetch_add(1, cc::memory_order_relaxed);
    }
    _wait_cv.notify_one();
}

cc::async_node_ptr cc::async_thread_pool::try_get_work(worker& w, bool authoritative)
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
        // Bounding this to a small fixed sample (4 of n, rather than 2n) looks tempting -- one idle worker's
        // scan is O(N) and N idle workers make it O(N^2). It measured badly on BOTH axes: an 8-node graph at 32
        // workers went 3.4 -> 23 us and the spawn tree 6.4 -> 7.0 ns/node, because a worker that samples 4 of 36
        // slots usually misses the one deque holding the work and sleeps instead. The scan is what finds work;
        // sampling it is the same mistake as backing it off (see async_pool_spin_rounds).
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

    // 3. the shared injection queue (foreign submits). Strong handles, so it stays a plain mutex — but taking
    //    that mutex on every scan is what made N idle workers contend on it, so it sits behind the hint (and,
    //    off the authoritative path, the poller token). See the members for why the two filters differ.
    auto const poll_injection = [&]() -> async_node_ptr
    {
        auto n = _injection.lock(
            [](cc::vector<async_node_ptr>& q) -> async_node_ptr
            {
                if (q.empty())
                    return nullptr;
                return q.pop_back();
            });
        if (n)
            _injection_hint.fetch_sub(1, cc::memory_order_relaxed);
        return n;
    };

    if (authoritative)
        return poll_injection();

    if (_injection_hint.load(cc::memory_order_relaxed) > 0 && _injection_poller.exchange(1, cc::memory_order_acquire) == 0)
    {
        auto n = poll_injection();
        _injection_poller.store(0, cc::memory_order_release);
        if (n)
            return n;
    }

    return nullptr;
}

// How many times an idle scanner re-scans before committing to the condvar. Used by both a worker and a
// foreign caller participating in participate_until_ready. Not a micro-optimization: a condvar round-trip is
// ~1-10 us while a spinning scanner picks work up in ~100 ns, and fork-join graphs run tasks that cost ~100 ns
// each and go briefly dry all the time -- sleeping instantly pays microseconds to save nanoseconds.
//
// Two variants were tried here and BOTH measured worse; the shape below is flat-pause-then-sleep on purpose
// (32 workers, spawn tree ns/node | 8-node graph us):
//
//   yield between scans        3.5 | 25.4   Great for the tree, catastrophic for latency, and no cadence fixed
//                                           it -- every 16th round still cost 24 us. The lesson: a yielding
//                                           worker is worse than a SLEEPING one. A sleeper gets notify_one'd;
//                                           a yielder is told nothing and must poll to notice, so it reacts
//                                           slower AND burns a syscall doing it.
//   exponential pause backoff  9.2 | ----   Same pause budget, ~7 scans instead of 128. A scan is not merely a
//                                           cost, it is what FINDS work -- backing it off blinds a worker while
//                                           fork-join is producing tasks every ~100 ns. PAUSE is the cheap part;
//                                           the SCAN budget is the one that buys throughput, so spend it.
//
// One case still prefers yielding: the spawn tree (trivial leaves, so scheduling IS the workload) wants idle
// workers off-CPU and gets 3.5 vs 6.1 ns/node. It is the only one -- quicksort and reduction are both BETTER
// without. Buying it back without the latency cost needs bounded searchers (one hot scanner, the rest properly
// asleep), which is a design change, not a constant.
static constexpr int async_pool_spin_rounds = 64;

void cc::async_thread_pool::worker_main(worker& w)
{
    cc::set_current_thread_name("async-pool");
    s_current_worker = &w;
    async_worker_scope const scope(*this);

    while (!_stop.load(cc::memory_order_acquire))
    {
        if (auto n = try_get_work(w))
        {
            n->poll();
            continue;
        }

        // 1. nothing right now -- scan a bounded while before paying for a sleep. No shared writes here, just
        //    re-scans, so a worker that is about to be handed work costs nobody anything. See the constant for
        //    the two backoff schemes that were tried and lost.
        bool found = false;
        for (int i = 0; i < async_pool_spin_rounds && !_stop.load(cc::memory_order_relaxed); ++i)
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
        cc::i64 const epoch = _wake_epoch.load(cc::memory_order_acquire);

        // Our half of the Dekker (see the protocol block). The RMW is a full barrier in its own right; the fence
        // spells out what it is for and keeps the pairing legible if the RMW is ever weakened.
        _sleepers.fetch_add(1, cc::memory_order_seq_cst);
        cc::atomic_thread_fence(cc::memory_order_seq_cst);

        // 3. the re-scan that closes the race: a producer that pushed before our registration became visible may
        //    have already read _sleepers == 0 and skipped notifying us. Seq_cst says at least one of us sees the
        //    other -- so if they missed us, we see their work here.
        // authoritative: this scan is what decides we may sleep, so it must not let the poller token filter it
        if (auto n = try_get_work(w, /*authoritative*/ true))
        {
            _sleepers.fetch_sub(1, cc::memory_order_relaxed);
            n->poll();
            continue;
        }

        {
            std::unique_lock<std::mutex> lk(_wait_m);
            _wait_cv.wait(
                lk, [&]
                { return _stop.load(cc::memory_order_relaxed) || _wake_epoch.load(cc::memory_order_relaxed) != epoch; });
        }
        _sleepers.fetch_sub(1, cc::memory_order_relaxed);
    }

    s_current_worker = nullptr;
}

// Park the calling thread until `root` completes. Does NOT schedule it — the caller has already placed it (and
// re-scheduling a running node would only force a redundant re-poll).
void cc::async_thread_pool::wait_for_completion(async_node_base& root)
{
    struct sync
    {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
    };
    sync s;

    // notify UNDER the lock so this hook (running on a worker) fully returns before wait_for_completion's frame
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

    if (already)
        return; // completed before we installed the hook: no wait, no notify pending

    std::unique_lock<std::mutex> lk(s.m);
    s.cv.wait(lk, [&] { return s.done; });
}

cc::async_thread_pool::worker* cc::async_thread_pool::try_claim_external_slot()
{
    for (int i = _thread_count; i < int(_workers.size()); ++i)
    {
        bool expected = false;
        if (_workers[i]->claimed.compare_exchange_strong(expected, true, cc::memory_order_acquire,
                                                         cc::memory_order_relaxed))
            return _workers[i].get();
    }
    return nullptr; // every slot is busy; the caller falls back to submit-and-park
}

// Hand whatever is still queued on a departing external slot back to the pool.
//
// This is not an optimization, it is the correctness cost of borrowing a slot. The deque dies with the drive,
// and a node left `scheduled` in it would be stranded forever: schedule()/schedule_on() are idempotent on
// `scheduled`, so no other scheduler could ever reclaim it and a blocking_get on that node would hang. (The
// singlethreaded_scheduler pays the same debt by drain()ing — see the note on its try_blocking_get.) Unlike the
// destructor's drain, which abandons work deliberately, this must RE-HOME it: the graph is still live.
void cc::async_thread_pool::drain_slot_to_injection(worker& w)
{
    // try_take is owner-only, and for an external slot the owner is this thread. Thieves may race us for the
    // top end; that is exactly what Chase-Lev's take/steal protocol resolves.
    async_node_base* raw = nullptr;
    bool any = false;
    while (w.deque.try_take(raw))
    {
        _injection.lock([&](cc::vector<async_node_ptr>& q) { q.push_back(async_node_ptr::adopt(raw)); });
        _injection_hint.fetch_add(1, cc::memory_order_relaxed);
        any = true;
    }
    if (any)
        wake_one();
}

void cc::async_thread_pool::participate_until_ready(async_node_base& root)
{
    worker* const slot = try_claim_external_slot();
    if (slot == nullptr)
    {
        // No free slot: fall back to handing the root over and parking, the pre-participation behavior.
        root.schedule_on(*this);
        wait_for_completion(root);
        return;
    }

    worker* const previous = s_current_worker;
    s_current_worker = slot;
    {
        async_worker_scope const scope(*this); // binds THIS pool, so the root's children route to our own deque

        // Drive the root HERE rather than schedule() it. Publishing it would push it onto our deque, wake a
        // worker for it, and then race that worker to take it back — for a graph we are about to run anyway.
        // At 32 workers the thief usually wins, and the caller ends up parked on a node it could have run
        // inline (measured: 328 ns -> 1535 ns for a single-node graph). Anything the root forks off is still
        // published normally by the poll loop, so a real graph still spreads.
        root.poll();

        while (!root.is_ready())
        {
            if (auto n = try_get_work(*slot))
            {
                n->poll();
                continue;
            }

            // Dry. Spin like a worker before giving up: the rest of the graph is in flight on the pool and work
            // may come back to us within nanoseconds, whereas parking costs microseconds.
            bool found = false;
            for (int i = 0; i < async_pool_spin_rounds && !root.is_ready(); ++i)
            {
                cc::spin_pause();
                if (auto n = try_get_work(*slot))
                {
                    n->poll();
                    found = true;
                    break;
                }
            }
            if (found || root.is_ready())
                continue;

            // Genuinely nothing for us and the root is still out there — park on it rather than burn a core the
            // pool wants. Anything we queued stays stealable while we sleep.
            wait_for_completion(root);
            break;
        }

        drain_slot_to_injection(*slot);
    }
    s_current_worker = previous;
    slot->claimed.store(false, cc::memory_order_release);
}

#else // CC_HAS_THREADS == 0

// ============================================================================
// The pool without threads
// ============================================================================
// Same class, same public API, no threads: cc::async_thread_pool exists on every platform, because gating the
// API on the flag would push the branch into every caller (the cc::threaded_actor policy). What it cannot do
// is run anything concurrently, so it stops pretending to.
//
// The one decision that does all the work is async_scheduler(false) -- no steal-capable peers. The poll loop
// publishes a node's dependencies only for peers to steal (see async.cc), so with none it drives the whole
// graph inline on the caller's stack and _queue stays empty on the common path. Everything else follows:
// nothing to steal means no deques, nobody to wake means no wake protocol, and nobody to park behind means no
// condvar. What is left is a LIFO queue and a pump -- which is cc::singlethreaded_scheduler, reached through
// the pool's API.
//
// The limit is real and worth stating: a graph parked on work only another thread could deliver never
// completes here. blocking_get's is_ready() assert reports that instead of hanging, which is the honest
// failure -- there is no thread that could ever arrive to make it true.

cc::async_thread_pool::async_thread_pool(int worker_count) : async_scheduler(false)
{
    // The count is accepted and ignored rather than asserted on: callers pass hardware_concurrency-shaped
    // numbers unconditionally, and refusing them here would be exactly the platform branch this fallback
    // exists to remove. Note there is no `>= 1` assert either — 0 workers is what this build always has.
    (void)worker_count;
}

cc::async_thread_pool::~async_thread_pool()
{
    CC_ASSERT(async_scheduler::default_or_null() != static_cast<async_scheduler*>(this),
              "uninstall this pool as the default before destroying it (uninstall_default_async_pool / "
              "scoped_default_async_pool)");

    // No drain by hand, unlike the threaded destructor: _queue holds real handles, so abandoned work releases
    // its own counts when the vector dies. Same contract though — outstanding graphs are dropped, not run.
}

int cc::async_thread_pool::default_worker_count()
{
    return 0; // no threads to give: the blocking_get caller is the only worker there is
}

void cc::async_thread_pool::enqueue(async_node_ptr node)
{
    CC_ASSERT(node != nullptr, "cannot enqueue a null node");
    // No "must be called from a worker of this pool" assert: there are no workers, and the one thread there is
    // may always queue. LIFO on the back — see the member.
    _queue.push_back(cc::move(node));
}

void cc::async_thread_pool::submit(async_node_ptr node)
{
    CC_ASSERT(node != nullptr, "cannot submit a null node");
    // The threaded pool splits submit from enqueue to keep foreign pushes off the workers' deques. With one
    // thread there is no foreign, and no injection queue to route to.
    _queue.push_back(cc::move(node));
}

void cc::async_thread_pool::participate_until_ready(async_node_base& root)
{
    async_worker_scope const scope(*this); // bind the pool, so nodes the frames touch route back to _queue

    // Drive the root here rather than schedule() it, for the same reason the threaded path does: it is work we
    // are about to do anyway. There, publishing races a thief for it; here it would just be a queue round trip.
    root.poll();

    // Then pump whatever it queued. Anything reachable runs, so falling out with the root not ready means the
    // graph is parked on something no thread here will ever deliver -- blocking_get's assert names that.
    while (!root.is_ready() && !_queue.empty())
    {
        async_node_ptr n = cc::move(_queue.back());
        _queue.pop_back();
        n->poll();
    }
}

#endif // CC_HAS_THREADS

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
