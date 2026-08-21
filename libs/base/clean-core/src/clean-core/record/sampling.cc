#include "sampling.hh"

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
#endif

using namespace cc::primitive_defines;

namespace
{
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
    u32 thread_index = 0;
    u32 chunk_offset = 0;
    u64 chunk_seq = 0;
    cc::stack_capture_result capture;
};

[[nodiscard]] bool is_sampleable(cc::rec::impl::thread_state const& ts)
{
    if (!ts.is_alive.load(cc::memory_order_acquire) || ts.tls == nullptr)
        return false;

    // Sampling the consumer is noise, and sampling it mid-dispatch is self-referential noise.
    return cc::string_view(ts.name) != actor_thread_name && cc::string_view(ts.name) != sampler_thread_name;
}

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

/// Suspends `handle`, fills `out`, and resumes it.
/// Everything between the suspend and the resume is a stack read into `frames`; nothing allocates and nothing locks.
[[nodiscard]] bool sample_suspended(void* handle,
                                    cc::rec::impl::thread_state const& ts,
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
            read_anchor(ts, out);

            void const* stop = nullptr;
            if (stop_at_scope && ts.tls != nullptr)
                stop = ts.tls->scope_frame;

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

    auto const frame_count = u32(count);
    cc::memcpy(out.data() + 16, &frame_count, sizeof(frame_count));
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
        auto sampled = false;

        isize total = 0;
#if defined(_WIN32)
        // Under the registry lock for the whole suspend: a thread_state is reaped once its owner dies and drains, and
        // the target must not be freed between the pick and the read.
        total = cc::rec::impl::with_nth_thread_state(cursor,
                                                     [&](cc::rec::impl::thread_state& ts)
                                                     {
                                                         if (!is_sampleable(ts))
                                                             return;

                                                         if (auto* const h = handles.get(ts.native_tid); h != nullptr)
                                                             sampled
                                                                 = sample_suspended(h, ts, frames, cfg.stop_at_scope, s);
                                                     });
#endif
        (void)total;

        ++cursor;

        if (sampled)
        {
            write_sample(s, frames);
            g_taken.fetch_add(1, cc::memory_order_relaxed);
        }
        else
            g_idle.fetch_add(1, cc::memory_order_relaxed);
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
        .field_count = 4,
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
