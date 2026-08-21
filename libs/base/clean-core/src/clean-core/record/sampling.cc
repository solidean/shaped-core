#include "sampling.hh"

#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/record/chunk.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/impl/system_state.hh>
#include <clean-core/record/impl/thread_state.hh>
#include <clean-core/record/impl/writer_tls.hh>
#include <clean-core/record/scope.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/writer.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh>

#if CC_HAS_THREADS
#include <thread>
#endif

#if defined(_WIN32)
#include <clean-core/platform/win32_sanitized.hh>
#include <tlhelp32.h> // the thread snapshot, which <Windows.h> does not pull in under LEAN_AND_MEAN
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

cc::rec::sampling_config g_config;
cc::atomic<bool> g_running = false;
cc::atomic<bool> g_stop = false;

cc::atomic<u64> g_taken = 0;
cc::atomic<u64> g_failed = 0;
cc::atomic<u64> g_idle = 0;

/// The anchor plus the frames, filled while the target is suspended and written once it is not.
struct sample
{
    u32 thread_index = cc::rec::impl::sample_unknown_thread;
    u32 chunk_offset = 0;
    u64 chunk_seq = 0;
    u64 native_tid = 0;
    cc::stack_capture_result capture;
};

[[nodiscard]] bool is_sampleable(cc::rec::impl::thread_state const& ts)
{
    if (!ts.is_alive.load(cc::memory_order_acquire) || ts.tls == nullptr)
        return false;

    // Sampling the consumer is noise, and sampling it mid-dispatch is self-referential noise.
    return cc::string_view(ts.name) != actor_thread_name && cc::string_view(ts.name) != sampler_thread_name;
}

#if defined(_WIN32)
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
    if (w == nullptr || w->current == nullptr)
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
/// `ts` is null for a thread the recorder has never heard of, which then contributes frames and nothing else.
[[nodiscard]] bool sample_suspended(void* handle,
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
                if (stop_at_scope)
                    stop = ts->tls != nullptr ? ts->tls->scope_frame : nullptr;
            }

            out.capture = cc::capture_stack_from_native_context(&ctx, frames, 0, stop);
            ok = out.capture.count > 0;
        }
    }

    ::ResumeThread(HANDLE(handle));
    return ok;
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

    auto writer = cc::rec::open_event(d, payload_bytes);
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
std::thread g_sampler;

void sampler_main()
{
    cc::set_current_thread_name(sampler_thread_name);
    cc::rec::set_current_thread_record_name(sampler_thread_name);

    auto const cfg = g_config;
    auto const interval = cfg.rate_hz > 0 ? 1.0 / cfg.rate_hz : 0.001;

    cc::vector<void*> frames;
    frames.resize_to_constructed(cc::max(cfg.max_frames, isize(1)), nullptr);

    cc::random rng(0x5A11u);
    isize cursor = 0; // where the round-robin got to, so no thread is starved by registration order

#if defined(_WIN32)
    handle_cache handles;
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
#endif

    while (!g_stop.load(cc::memory_order_acquire))
    {
        auto const wobble = cfg.jitter > 0 ? 1.0 + cfg.jitter * (rng.uniform(0.0, 2.0) - 1.0) : 1.0;
#if defined(_WIN32)
        timer.sleep(interval * wobble);
#else
        cc::this_thread_sleep_secs(interval * wobble);
#endif

        if (g_stop.load(cc::memory_order_acquire))
            break;

        sample s;

#if defined(_WIN32)
        if (cfg.include_unknown_threads && cc::current_time_steady_secs() >= next_refresh_at)
        {
            os_threads.refresh(self_tid);
            next_refresh_at = cc::current_time_steady_secs() + refresh_interval_secs;
        }

        // One round-robin over both halves, so a thread with no stream competes for slots on equal terms.
        // The two differ only in whether there is a stream to anchor into; the walk is the same.
        auto const unknown_count = cfg.include_unknown_threads ? os_threads.tids.size() : isize(0);
        auto const total_targets = known_count + unknown_count;
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

                if (slot < known_count || unknown_count == 0)
                {
                    known_count = cc::rec::impl::with_nth_thread_state(
                        slot,
                        [&](cc::rec::impl::thread_state& ts)
                        {
                            if (!is_sampleable(ts))
                                return;

                            s.native_tid = ts.native_tid;
                            if (auto* const h = handles.get(ts.native_tid); h != nullptr)
                                took = sample_suspended(h, &ts, frames, cfg.stop_at_scope, s);
                        });
                }
                else
                {
                    auto const tid = os_threads.tids[slot - known_count];
                    s.native_tid = tid;
                    if (auto* const h = handles.get(tid); h != nullptr)
                        took = sample_suspended(h, nullptr, frames, cfg.stop_at_scope, s);
                }

                if (took)
                {
                    // Inside the tick scope, so the sample and its cost stay together until splicing moves the sample
                    // onto the thread it caught.
                    write_sample(s, frames);
                    g_taken.fetch_add(1, cc::memory_order_relaxed);
                }
                else
                    g_idle.fetch_add(1, cc::memory_order_relaxed);
            }
        }
#endif

        cursor += per_tick > 0 ? per_tick : 1;
    }

#if defined(_WIN32)
    handles.clear();
#endif
}
#endif
} // namespace

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
    if (!rec::is_initialized() || !cc::stack_capture_from_context_available())
        return;

    rec::stop_sampling();

    g_config = cfg;
    g_taken.store(0, cc::memory_order_relaxed);
    g_failed.store(0, cc::memory_order_relaxed);
    g_idle.store(0, cc::memory_order_relaxed);
    g_stop.store(false, cc::memory_order_release);
    g_running.store(true, cc::memory_order_release);

    g_sampler = std::thread(sampler_main);
#endif
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
    };
}
