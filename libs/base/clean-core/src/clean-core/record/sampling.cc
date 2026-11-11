#include "sampling.hh"

#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/record/chunk.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/impl/system_state.hh>
#include <clean-core/record/impl/thread_state.hh>
#include <clean-core/record/impl/writer_tls.hh>
#include <clean-core/record/log.hh>
#include <clean-core/record/scope.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/writer.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/native_thread.hh>
#include <clean-core/thread/thread.hh>

#if defined(_WIN32)
#include <clean-core/platform/win32_sanitized.hh>
#include <tlhelp32.h> // the thread snapshot, which <Windows.h> does not pull in under LEAN_AND_MEAN
#elif defined(__APPLE__)
#include <mach/mach.h> // task_threads, thread_suspend, thread_get_state: walking a thread from outside
#include <pthread.h>   // a Mach thread's pthread, which knows its stack's bounds
#endif

using namespace cc::primitive_defines;

namespace
{
// The sampler's own events are recorder bookkeeping, so they answer to the system domain rather than to whatever a
// caller's default happens to be.
// In the anonymous namespace on purpose: the macros look the name up unqualified, so this shadows the global fallback
// for this file and collides with nothing.
[[nodiscard]] constexpr cc::rec::domain* cc_rec_domain()
{
    return &cc::rec::g_system_domain;
}

/// The order everything below is written to obey.
///
/// 1. Take the registry lock, so the target's thread_state cannot be reaped underneath us.
/// 2. Suspend the target.
/// 3. Read its anchor and walk its stack into a FIXED buffer — no allocation, no lock, nothing that can block.
/// 4. Resume it.
/// 5. Release the lock, and only then write the event.
///
/// Steps 3 and 5 are the two that would deadlock if reordered.
/// A suspended thread may hold the allocator's heap lock, so allocating before resuming hangs; it may hold the chunk
/// pool's lock, so writing an event before resuming hangs on the same lock the target is holding.

constexpr char const* sampler_thread_name = "cc-sample";
constexpr char const* actor_thread_name = "cc-record";

/// The live configuration, which a UI or a test may replace while the sampler runs.
///
/// Behind a lock rather than a pile of atomics: it is read once per tick, half a millisecond apart at the fastest,
/// so the lock is free and the config stays one coherent object rather than fields that can disagree mid-tick.
cc::mutex<cc::rec::sampling_config> g_config;

[[nodiscard]] cc::rec::sampling_config load_config()
{
    return g_config.lock([](cc::rec::sampling_config const& c) { return c; });
}
cc::atomic<bool> g_running = false;
cc::atomic<bool> g_stop = false;

cc::atomic<u64> g_taken = 0;
cc::atomic<u64> g_failed = 0;
cc::atomic<u64> g_idle = 0;
cc::atomic<u64> g_ticks = 0;

/// The anchor plus the frames, filled while the target is suspended and written once it is not.
struct sample
{
    u32 thread_index = cc::rec::impl::sample_unknown_thread;
    u32 chunk_offset = 0;
    u64 chunk_seq = 0;
    u64 native_tid = 0;
    u64 owner = 0;
    cc::stack_capture_result capture;
    /// Whether the thread had a recorded scope open, read while it was suspended.
    bool in_scope = false;
};

[[nodiscard]] bool is_sampleable(cc::rec::impl::thread_state const& ts)
{
    if (!ts.is_alive.load(cc::memory_order_acquire) || ts.tls == nullptr)
        return false;

    // Sampling the consumer is noise, and sampling it mid-dispatch is self-referential noise.
    return cc::string_view(ts.name) != actor_thread_name && cc::string_view(ts.name) != sampler_thread_name;
}

#if defined(_WIN32) || defined(__APPLE__)
/// Every OS thread the recorder already knows, collected in ONE pass under the registry lock.
///
/// Asking per thread instead would take the lock once per thread in the process, on a path that already costs a
/// snapshot — and the sampler holding that lock repeatedly delays every thread trying to register.
void collect_registered(cc::vector<u64>& out)
{
    out.clear();
    cc::rec::impl::for_each_thread_state(
        [&](cc::rec::impl::thread_state& ts)
        {
            if (ts.is_alive.load(cc::memory_order_acquire))
                out.push_back(ts.native_tid);
        });
}
#endif

/// Reads where the target's stream had got to.
/// Only meaningful while the target is suspended; a running thread's cursor moves under the read.
void read_anchor(cc::rec::impl::thread_state const& ts, sample& out)
{
    out.thread_index = ts.index;

    auto const* const w = ts.tls;
    if (w == nullptr)
        return;

    // What its next event would carry: a pending reset means it has left every context.
    out.owner = w->ambient_reset_pending ? 0 : w->last_owner;

    if (w->current == nullptr)
        return; // a thread that has recorded nothing has no position, and no state to recover either

    out.chunk_seq = w->current->seq;
    out.chunk_offset = w->current->committed.load(cc::memory_order_acquire);
}

#if defined(_WIN32)
#if !defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/// A sleep that can actually mean a millisecond.
///
/// The default Windows timer resolution is ~15.6 ms, so an ordinary sleep silently caps a 1 kHz sampler at about
/// 64 Hz — a profiler that reports a rate it is not achieving is worse than one that admits it cannot.
/// A high-resolution waitable timer gets sub-millisecond precision without timeBeginPeriod, which would coarsen or
/// sharpen the whole process for everyone else.
struct precise_timer
{
    precise_timer()
    {
        _h = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (_h == nullptr) // pre-1803, where the flag is rejected
            _h = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~precise_timer()
    {
        if (_h != nullptr)
            ::CloseHandle(_h);
    }

    precise_timer(precise_timer const&) = delete;
    precise_timer& operator=(precise_timer const&) = delete;

    void sleep(f64 secs) const
    {
        if (_h == nullptr || secs <= 0)
        {
            cc::this_thread_sleep_secs(secs);
            return;
        }

        LARGE_INTEGER due;
        due.QuadPart = -LONGLONG(secs * 1e7); // negative is relative, in 100ns units
        if (::SetWaitableTimer(_h, &due, 0, nullptr, nullptr, FALSE) == 0)
        {
            cc::this_thread_sleep_secs(secs);
            return;
        }

        ::WaitForSingleObject(_h, INFINITE);
    }

private:
    HANDLE _h = nullptr;
};

/// Thread handles, kept rather than reopened: OpenThread costs about as much as the suspend it precedes.
struct handle_cache
{
    cc::vector<u64> tids;
    cc::vector<void*> handles;

    [[nodiscard]] void* get(u64 native_tid)
    {
        for (isize i = 0; i < tids.size(); ++i)
            if (tids[i] == native_tid)
                return handles[i];

        auto* const h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                     DWORD(native_tid));
        if (h == nullptr)
            return nullptr;

        tids.push_back(native_tid);
        handles.push_back(h);
        return h;
    }

    void clear()
    {
        for (auto* const h : handles)
            ::CloseHandle(HANDLE(h));
        tids.clear();
        handles.clear();
    }
};

/// Every thread in this process, refreshed occasionally rather than per tick.
///
/// A snapshot costs on the order of a millisecond, which is a whole tick at 1 kHz, so doing it per sample would spend
/// most of the sampler's budget deciding whom to sample.
struct os_thread_list
{
    cc::vector<u64> tids;

private:
    cc::vector<u64> _registered;

public:
    /// Keeps only the threads the recorder does NOT know, so a round-robin slot is never spent on a thread the
    /// known half already covers.
    void refresh(u64 exclude)
    {
        tids.clear();
        collect_registered(_registered);

        auto* const snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;

        auto const self = ::GetCurrentProcessId();

        THREADENTRY32 entry = {};
        entry.dwSize = sizeof(entry);
        if (::Thread32First(snap, &entry) != 0)
            do
            {
                // The snapshot spans every process, and a thread that is not ours cannot be suspended anyway.
                if (entry.th32OwnerProcessID != self)
                    continue;
                if (u64(entry.th32ThreadID) == exclude)
                    continue; // suspending the sampler would suspend the thing doing the suspending
                auto known = false;
                for (auto const r : _registered)
                    if (r == u64(entry.th32ThreadID))
                    {
                        known = true;
                        break;
                    }
                if (known)
                    continue;

                tids.push_back(u64(entry.th32ThreadID));
            } while (::Thread32Next(snap, &entry) != 0);

        ::CloseHandle(snap);
    }
};

/// Suspends `handle`, fills `out`, and resumes it.
/// Everything between the suspend and the resume is a stack read into `frames`; nothing allocates and nothing locks.
/// That includes the OS's own function lookup, whose lock a thread suspended mid-DLL-load holds: `unwind` stands in.
/// `ts` is null for a thread the recorder has never heard of, which then contributes frames and nothing else.
[[nodiscard]] bool sample_suspended(void* handle,
                                    cc::unwind_snapshot const& unwind,
                                    cc::rec::impl::thread_state const* ts,
                                    cc::span<void*> frames,
                                    bool stop_at_scope,
                                    sample& out)
{
    if (::SuspendThread(HANDLE(handle)) == DWORD(-1))
        return false;

    auto ok = false;
    {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (::GetThreadContext(HANDLE(handle), &ctx) != 0)
        {
            void const* stop = nullptr;
            if (ts != nullptr)
            {
                read_anchor(*ts, out);
                out.in_scope = ts->tls != nullptr && ts->tls->scope_frame != nullptr;
                if (stop_at_scope)
                    stop = ts->tls != nullptr ? ts->tls->scope_frame : nullptr;
            }

            out.capture = cc::capture_stack_from_native_context(&ctx, unwind, frames, 0, stop);
            ok = out.capture.count > 0;
        }
    }

    ::ResumeThread(HANDLE(handle));
    return ok;
}
#endif

#if defined(__APPLE__) && CC_HAS_THREADS
/// One thread of this process as Mach names it: a port to suspend it by, and its own stack's bounds.
struct mach_thread
{
    u64 tid = 0;
    thread_act_t port = MACH_PORT_NULL;
    u64 stack_low = 0;
    u64 stack_high = 0;
};

/// Every thread of this process, listed on a wall-clock interval rather than per tick.
///
/// The bounds are read here, while every thread runs: asking for them goes through the pthread list's lock, which a
/// suspended thread could hold.
/// A thread no pthread owns keeps bounds of 0, and is skipped, since its chain cannot be checked against anything.
struct mach_threads
{
    cc::vector<mach_thread> threads;

    mach_threads() = default;
    mach_threads(mach_threads const&) = delete;
    mach_threads& operator=(mach_threads const&) = delete;
    ~mach_threads() { clear(); }

    void refresh(u64 exclude)
    {
        clear();
        thread_act_array_t list = nullptr;
        auto count = mach_msg_type_number_t(0);
        if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS)
            return;

        for (auto i = mach_msg_type_number_t(0); i < count; ++i)
        {
            auto t = mach_thread{.port = list[i]};
            auto identifier = thread_identifier_info_data_t{};
            auto info_count = mach_msg_type_number_t(THREAD_IDENTIFIER_INFO_COUNT);
            if (thread_info(list[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&identifier), &info_count)
                == KERN_SUCCESS)
                t.tid = identifier.thread_id;

            // suspending the sampler would suspend the thing doing the suspending
            if (t.tid == 0 || t.tid == exclude)
            {
                (void)mach_port_deallocate(mach_task_self(), list[i]);
                continue;
            }

            if (auto const owner = pthread_from_mach_thread_np(list[i]); owner != nullptr)
            {
                t.stack_high = u64(reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(owner)));
                t.stack_low = t.stack_high - u64(pthread_get_stacksize_np(owner));
            }
            threads.push_back(t);
        }
        (void)vm_deallocate(mach_task_self(), vm_address_t(list), vm_size_t(count * sizeof(thread_act_t)));
    }

    [[nodiscard]] mach_thread* find(u64 tid)
    {
        for (auto& t : threads)
            if (t.tid == tid)
                return &t;
        return nullptr;
    }

    void clear()
    {
        for (auto const& t : threads)
            (void)mach_port_deallocate(mach_task_self(), t.port);
        threads.clear();
    }
};

/// What the sampler loop suspends a thread by: here, its entry in the Mach list.
struct handle_cache
{
    mach_threads* table = nullptr;

    [[nodiscard]] void* get(u64 native_tid) { return table->find(native_tid); }
    void clear() {}
};

/// The threads the recorder does NOT know, so a round-robin slot is never spent on one the known half covers.
struct os_thread_list
{
    cc::vector<u64> tids;

private:
    cc::vector<u64> _registered;

public:
    void refresh(mach_threads const& table)
    {
        tids.clear();
        collect_registered(_registered);
        for (auto const& t : table.threads)
        {
            auto known = false;
            for (auto const r : _registered)
                known = known || r == t.tid;
            if (!known)
                tids.push_back(t.tid);
        }
    }
};

/// Suspends the thread `handle` names, fills `out`, and resumes it.
/// Between the suspend and the resume only its registers and its own stack are read, into `frames`: nothing allocates
/// and nothing locks.
/// `ts` is null for a thread the recorder has never heard of, which then contributes frames and nothing else.
[[nodiscard]] bool sample_suspended(void* handle,
                                    cc::unwind_snapshot const&,
                                    cc::rec::impl::thread_state const* ts,
                                    cc::span<void*> frames,
                                    bool stop_at_scope,
                                    sample& out)
{
    auto const& t = *static_cast<mach_thread const*>(handle);
    if (t.stack_low == 0 || thread_suspend(t.port) != KERN_SUCCESS)
        return false;

    auto ok = false;
#if defined(__aarch64__)
    auto state = arm_thread_state64_t{};
    auto count = mach_msg_type_number_t(ARM_THREAD_STATE64_COUNT);
    auto const read = thread_get_state(t.port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count);
    auto const fp = read == KERN_SUCCESS ? u64(arm_thread_state64_get_fp(state)) : u64(0);
    auto const pc = read == KERN_SUCCESS ? u64(arm_thread_state64_get_pc(state)) : u64(0);
#else
    auto state = x86_thread_state64_t{};
    auto count = mach_msg_type_number_t(x86_THREAD_STATE64_COUNT);
    auto const read = thread_get_state(t.port, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count);
    auto const fp = read == KERN_SUCCESS ? u64(state.__rbp) : u64(0);
    auto const pc = read == KERN_SUCCESS ? u64(state.__rip) : u64(0);
#endif
    if (read == KERN_SUCCESS)
    {
        void const* stop = nullptr;
        if (ts != nullptr)
        {
            read_anchor(*ts, out);
            out.in_scope = ts->tls != nullptr && ts->tls->scope_frame != nullptr;
            if (stop_at_scope)
                stop = ts->tls != nullptr ? ts->tls->scope_frame : nullptr;
        }
        out.capture = cc::capture_stack_from_frame(reinterpret_cast<void const*>(fp), reinterpret_cast<void*>(pc),
                                                   t.stack_low, t.stack_high, frames, stop);
        ok = out.capture.count > 0;
    }

    (void)thread_resume(t.port);
    return ok;
}
#endif

#if defined(_WIN32) || (defined(__APPLE__) && CC_HAS_THREADS)
/// Where a thread's running time stood at the sampler's previous tick of it, both in the timestamp counter's ticks.
struct run_mark
{
    u64 tsc = 0;
    u64 cycles = 0;
};

/// How much of a stretch a thread ran, both in the timestamp counter's ticks; `interval` 0 when there is none to tell.
struct thread_run
{
    u64 interval = 0;
    u64 ran = 0;
};

/// The stretch since `mark` given the thread's running time now, `cycles`, moving `mark` to now.
thread_run advance_run_mark(run_mark& mark, u64 cycles)
{
    auto const tsc = cc::current_cycles();
    auto const first = mark.tsc == 0;
    auto const out = thread_run{.interval = tsc - mark.tsc, .ran = cycles - mark.cycles};
    mark = {.tsc = tsc, .cycles = cycles};
    return first ? thread_run{} : out;
}
#endif

#if defined(_WIN32)

/// How much of the stretch since `mark` the thread behind `handle` ran, moving `mark` to now.
/// QueryThreadCycleTime counts in the timestamp counter's ticks, so the two are one unit; the thread need not be
/// suspended for it, and is read before the sampler suspends it, so the walk does not count as time it did not run.
thread_run read_thread_run(void* handle, run_mark& mark)
{
    auto cycles = ULONG64(0);
    if (::QueryThreadCycleTime(HANDLE(handle), &cycles) == 0)
        return {};
    return advance_run_mark(mark, u64(cycles));
}
#elif defined(__APPLE__) && CC_HAS_THREADS
/// How much of the stretch since `mark` the thread behind `handle` ran, moving `mark` to now.
/// THREAD_EXTENDED_INFO counts user and system time in nanoseconds, converted here to the counter's ticks at the
/// recorder's calibrated rate, so `ran` and `interval` share the unit they have on Windows.
/// THREAD_BASIC_INFO would give the same in whole microseconds, a tenth of a 1 kHz tick.
/// The thread need not be suspended for it, and is read before the sampler suspends it.
thread_run read_thread_run(void* handle, run_mark& mark)
{
    auto const rate = cc::rec::cycles_per_second();
    if (rate <= 0)
        return {};
    auto info = thread_extended_info_data_t{};
    auto count = mach_msg_type_number_t(THREAD_EXTENDED_INFO_COUNT);
    auto const port = static_cast<mach_thread*>(handle)->port;
    if (thread_info(port, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS)
        return {};
    auto const ran_ns = info.pth_user_time + info.pth_system_time;
    return advance_run_mark(mark, u64(f64(ran_ns) * rate * 1e-9));
}
#endif

#if defined(_WIN32) || (defined(__APPLE__) && CC_HAS_THREADS)
/// Records `run` for a thread, with whether the walk that followed found a recorded scope open.
void record_thread_run(thread_run const& run, u32 thread_index, u64 native_tid, bool in_scope)
{
    auto const& d = cc::rec::impl::thread_run_desc();
    if (run.interval == 0 || !cc::rec::is_recording(d))
        return;
    auto const interval = run.interval;
    auto const ran = run.ran;
    auto writer = cc::rec::open_event(d, cc::rec::impl::thread_run_payload_size, cc::rec::impl::thread_run_payload_size);
    if (!writer.is_open())
        return;
    auto const out = writer.payload();
    if (out.size() < cc::rec::impl::thread_run_payload_size)
        return;
    auto const scope_flag = u32(in_scope ? 1 : 0);
    cc::memcpy(out.data() + 0, &thread_index, sizeof(thread_index));
    cc::memcpy(out.data() + 4, &scope_flag, sizeof(scope_flag));
    cc::memcpy(out.data() + 8, &native_tid, sizeof(native_tid));
    cc::memcpy(out.data() + 16, &interval, sizeof(interval));
    cc::memcpy(out.data() + 24, &ran, sizeof(ran));
    writer.commit(cc::rec::impl::thread_run_payload_size, cc::rec::impl::flag_none);
}
#endif

/// Writes one sample into the sampler's own stream.
void write_sample(sample const& s, cc::span<void* const> frames)
{
    auto const& d = cc::rec::impl::sample_desc();
    if (!cc::rec::is_recording(d))
        return;

    auto const count = s.capture.count;
    auto const payload_bytes = cc::rec::impl::sample_frames_offset + count * isize(sizeof(u64));

    auto writer = cc::rec::open_event(d, payload_bytes, payload_bytes);
    if (!writer.is_open())
        return;

    auto const out = writer.payload();
    if (out.size() < payload_bytes)
        return;

    cc::memcpy(out.data() + 0, &s.thread_index, sizeof(s.thread_index));
    cc::memcpy(out.data() + 4, &s.chunk_offset, sizeof(s.chunk_offset));
    cc::memcpy(out.data() + 8, &s.chunk_seq, sizeof(s.chunk_seq));
    cc::memcpy(out.data() + 16, &s.native_tid, sizeof(s.native_tid));

    auto const frame_count = u32(count);
    cc::memcpy(out.data() + 24, &frame_count, sizeof(frame_count));
    for (isize i = 0; i < count; ++i)
    {
        auto const address = reinterpret_cast<u64>(frames[i]);
        cc::memcpy(out.data() + cc::rec::impl::sample_frames_offset + i * isize(sizeof(u64)), &address, sizeof(address));
    }

    writer.commit(payload_bytes, s.capture.truncated ? cc::rec::impl::flag_truncated : cc::rec::impl::flag_none);
}

#if CC_HAS_THREADS
cc::native_thread g_sampler;

void sampler_main()
{
    cc::set_current_thread_name(sampler_thread_name);
    cc::rec::set_current_thread_record_name(sampler_thread_name);

    auto cfg = load_config();

    cc::vector<void*> frames;
    frames.resize_to_constructed(cc::max(cfg.max_frames, isize(1)), nullptr);

    cc::random rng(0x5A11u);

#if defined(_WIN32)
    isize cursor = 0; // where the round-robin got to, so no thread is starved by registration order

    handle_cache handles;
    auto run_marks
        = cc::map<u64, run_mark>(); // per thread the sampler knows, where its running time stood at its last tick
    precise_timer const timer;
    os_thread_list os_threads;
    auto const self_tid = cc::native_thread_id();
    auto known_count = isize(1); // corrected by the first known-half tick

    // Discovering unknown threads is rationed by WALL TIME, not by ticks, and never runs before the first interval.
    //
    // A thread snapshot enumerates every thread on the machine rather than just this process's, which measures in
    // milliseconds — many ticks at a kilohertz.
    // Doing it up front spends the whole sampling window of a short run on it, and doing it per N ticks halves the
    // rate on a long one; so a short run simply covers the threads the recorder already knows, which are the ones it
    // was told to care about anyway.
    constexpr f64 refresh_interval_secs = 0.1;
    auto next_refresh_at = cc::current_time_steady_secs() + refresh_interval_secs;

    // Taken again once a walk reaches a module it lacks, at most this often: code generated at run time is in no
    // module, and a stack through it would otherwise rebuild the snapshot on every tick.
    auto unwind = cc::unwind_snapshot::create();
    auto unwind_is_stale = false;
    constexpr f64 unwind_refresh_secs = 0.25;
    auto next_unwind_refresh_at = cc::current_time_steady_secs() + unwind_refresh_secs;
#elif defined(__APPLE__)
    isize cursor = 0; // where the round-robin got to, so no thread is starved by registration order

    // The Mach list is what both halves suspend by, so it is taken at once and then on the same wall-clock interval
    // the unknown half is: listing costs far less than a Windows snapshot, but it allocates a port per thread.
    mach_threads table;
    auto handles = handle_cache{.table = &table};
    auto run_marks
        = cc::map<u64, run_mark>(); // per thread the sampler knows, where its running time stood at its last tick
    os_thread_list os_threads;
    auto const self_tid = cc::native_thread_id();
    auto known_count = isize(1); // corrected by the first known-half tick
    constexpr f64 refresh_interval_secs = 0.1;
    table.refresh(self_tid);
    if (cfg.include_unknown_threads)
        os_threads.refresh(table);
    auto next_refresh_at = cc::current_time_steady_secs() + refresh_interval_secs;
    auto const unwind = cc::unwind_snapshot::create(); // empty: a frame chain needs no tables
    auto unwind_is_stale = false;
#endif

    while (!g_stop.load(cc::memory_order_acquire))
    {
        // Re-read every tick, so a UI checkbox or a scoped override takes effect within one interval rather than
        // needing the sampler stopped and restarted — which would throw away the intern table with it.
        cfg = load_config();
        if (frames.size() != cc::max(cfg.max_frames, isize(1)))
            frames.resize_to_constructed(cc::max(cfg.max_frames, isize(1)), nullptr);

        auto const interval = cfg.rate_hz > 0 ? 1.0 / cfg.rate_hz : 0.001;
        auto const wobble = cfg.jitter > 0 ? 1.0 + cfg.jitter * (rng.uniform(0.0, 2.0) - 1.0) : 1.0;
#if defined(_WIN32)
        timer.sleep(interval * wobble);
#else
        cc::this_thread_sleep_secs(interval * wobble);
#endif

        if (g_stop.load(cc::memory_order_acquire))
            break;

        sample s;

#if defined(_WIN32) || defined(__APPLE__)
#if defined(_WIN32)
        if (unwind_is_stale && cc::current_time_steady_secs() >= next_unwind_refresh_at)
        {
            unwind = cc::unwind_snapshot::create();
            unwind_is_stale = false;
            next_unwind_refresh_at = cc::current_time_steady_secs() + unwind_refresh_secs;
        }

        if (cfg.include_unknown_threads && cc::current_time_steady_secs() >= next_refresh_at)
        {
            os_threads.refresh(self_tid);
            next_refresh_at = cc::current_time_steady_secs() + refresh_interval_secs;
        }
#else
        if (cc::current_time_steady_secs() >= next_refresh_at)
        {
            table.refresh(self_tid);
            if (cfg.include_unknown_threads)
                os_threads.refresh(table);
            next_refresh_at = cc::current_time_steady_secs() + refresh_interval_secs;
        }
#endif

        // One round-robin over both halves, so a thread with no stream competes for slots on equal terms.
        // The two differ only in whether there is a stream to anchor into; the walk is the same.
        // Slots are laid out against the counts as of this tick: the known half answers its live count back, and a
        // set that shrank mid-tick must not shift an unknown slot past the end of `os_threads.tids`.
        auto const unknown_count = cfg.include_unknown_threads ? os_threads.tids.size() : isize(0);
        auto const known_at_tick = known_count;
        auto const total_targets = known_at_tick + unknown_count;
        auto const per_tick = cfg.threads_per_tick > 0 ? cc::min(cfg.threads_per_tick, total_targets) : total_targets;

        if (total_targets > 0)
        {
            // The sampler's own cost, on the sampler's own lane.
            //
            // Not vanity: a sampled profile is only as trustworthy as its cadence, and a reader judging whether a
            // 16 ms frame was sampled evenly or aliased against it needs to SEE when the ticks landed and what each
            // one cost.
            CC_RECORD_SCOPE("record.sample_tick");

            for (isize n = 0; n < per_tick; ++n)
            {
                auto const slot = (cursor + n) % total_targets;
                s = sample{};
                auto took = false;

                // Whether this slot named a thread worth sampling at all.
                // Without it a slot that landed on the actor — or on a thread that has since died — would be
                // indistinguishable from one whose suspend or walk failed, and only the second is a symptom.
                auto attempted = false;

                if (slot < known_at_tick || unknown_count == 0)
                {
                    known_count = cc::rec::impl::with_nth_thread_state(
                        slot,
                        [&](cc::rec::impl::thread_state& ts)
                        {
                            if (!is_sampleable(ts))
                                return;

                            attempted = true;
                            s.native_tid = ts.native_tid;
                            if (auto* const h = handles.get(ts.native_tid); h != nullptr)
                            {
                                auto const run
                                    = cfg.thread_run ? read_thread_run(h, run_marks[ts.native_tid]) : thread_run{};
                                took = sample_suspended(h, unwind, &ts, frames, cfg.stop_at_scope, s);
                                record_thread_run(run, ts.index, ts.native_tid, took && s.in_scope);
                            }
                        });
                }
                else
                {
                    auto const tid = os_threads.tids[slot - known_at_tick];
                    attempted = true;
                    s.native_tid = tid;
                    if (auto* const h = handles.get(tid); h != nullptr)
                        took = sample_suspended(h, unwind, nullptr, frames, cfg.stop_at_scope, s);
                }

                unwind_is_stale = unwind_is_stale || s.capture.left_snapshot;

                if (took)
                {
                    // Inside the tick scope, so the sample and its cost stay together until splicing moves the sample
                    // onto the thread it caught.
                    if (cfg.sink != nullptr)
                        cfg.sink(cc::rec::sample_view{.native_tid = s.native_tid,
                                                      .owner = s.owner,
                                                      .frames = cc::span<void* const>(frames.data(), s.capture.count),
                                                      .truncated = s.capture.truncated},
                                 cfg.sink_user);
                    else
                        write_sample(s, frames);
                    g_taken.fetch_add(1, cc::memory_order_relaxed);
                }
                else if (attempted)
                    g_failed.fetch_add(1, cc::memory_order_relaxed);
                else
                    g_idle.fetch_add(1, cc::memory_order_relaxed);
            }
        }

        // Inside the guard, with the `per_tick` it advances by: every target this loop knows about is Windows' or
        // macOS's, and elsewhere the tick body does nothing to advance past.
        cursor += per_tick > 0 ? per_tick : 1;
        g_ticks.fetch_add(1, cc::memory_order_relaxed);
#endif
    }

#if defined(_WIN32)
    handles.clear();
#endif
}
#endif
} // namespace

bool cc::rec::sampling_available()
{
#if CC_HAS_THREADS && (defined(_WIN32) || defined(__APPLE__))
    return true;
#else
    return false;
#endif
}

cc::rec::desc const& cc::rec::impl::thread_run_desc()
{
    static constexpr rec::desc d = {
        .kind = rec::event_kind::value,
        .enable_bit = rec::enable_bit_of(rec::category::profiling),
        .name = "record.thread_run",
        .dom = &cc::rec::g_system_domain,
        .fields = rec::impl::thread_run_fields,
        .field_count = u16(CC_ARRAY_COUNT_OF(rec::impl::thread_run_fields)),
        .fixed_payload_size = rec::impl::thread_run_payload_size,
    };
    return d;
}

cc::rec::desc const& cc::rec::impl::sample_desc()
{
    static constexpr rec::desc d = {
        .kind = rec::event_kind::sample,
        .enable_bit = rec::enable_bit_of(rec::category::profiling),
        .name = "record.sample",
        .dom = &cc::rec::g_system_domain,
        .fields = rec::impl::sample_fields,
        .field_count = u16(CC_ARRAY_COUNT_OF(rec::impl::sample_fields)),
        .fixed_payload_size = rec::desc::variable_payload,
    };
    return d;
}

void cc::rec::start_sampling(cc::rec::sampling_config const& cfg)
{
    // A sampler is one thing that genuinely cannot fall back to the calling thread: it exists to observe a thread
    // that is not this one, from outside.
    // So a build without threads has no sampler, and is_sampling() says so rather than pretending.
#if !CC_HAS_THREADS
    (void)cfg;
    return;
#else
    if (!rec::is_initialized())
        return;

    // **Refused rather than degraded**, and said out loud.
    //
    // Sampling is not a cost question a knob could settle here: the model is to suspend a thread and walk it from
    // outside, and a platform with no such operation cannot do a cheaper version of it.
    // wasm is the case — a worker can be walked by nobody but itself — and a silent return left a caller believing
    // a profile was being taken.
    if (!cc::rec::sampling_available())
    {
        CC_LOG_WARNING("sampling is unavailable: this platform cannot walk a thread other than the calling one");
        return;
    }

    rec::stop_sampling();

    g_config.lock([&](cc::rec::sampling_config& c) { c = cfg; });

    g_taken.store(0, cc::memory_order_relaxed);
    g_failed.store(0, cc::memory_order_relaxed);
    g_idle.store(0, cc::memory_order_relaxed);
    g_ticks.store(0, cc::memory_order_relaxed);
    g_stop.store(false, cc::memory_order_release);
    g_running.store(true, cc::memory_order_release);

    g_sampler = cc::native_thread::start([] { sampler_main(); });
#endif
}

void cc::rec::reconfigure_sampling(cc::rec::sampling_config const& cfg)
{
    g_config.lock([&](cc::rec::sampling_config& c) { c = cfg; });
}

cc::rec::sampling_config cc::rec::current_sampling_config()
{
    return load_config();
}

void cc::rec::stop_sampling()
{
    if (!g_running.load(cc::memory_order_acquire))
        return;

    g_stop.store(true, cc::memory_order_release);

#if CC_HAS_THREADS
    if (g_sampler.joinable())
        g_sampler.join();
#endif

    g_running.store(false, cc::memory_order_release);
}

bool cc::rec::is_sampling()
{
    return g_running.load(cc::memory_order_acquire);
}

cc::rec::sampling_stats cc::rec::sampling_statistics()
{
    return {
        .taken = g_taken.load(cc::memory_order_relaxed),
        .failed = g_failed.load(cc::memory_order_relaxed),
        .idle = g_idle.load(cc::memory_order_relaxed),
        .ticks = g_ticks.load(cc::memory_order_relaxed),
    };
}
