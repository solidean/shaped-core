#include <clean-core/container/map.hh>
#include <clean-core/record/async_scope.hh>
#include <clean-core/record/console_listener.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/serialize.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/trace.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/mutex.hh>
#include <nexus/impl/rec_session.hh>
#include <nexus/rec.hh>

using namespace cc::primitive_defines;

namespace nx
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "nexus");
} // namespace nx

namespace
{
/// One test's events, plus what to do with them when the test ends.
struct bucket
{
    /// Borrowed from the test declaration, which the registry owns for the whole run.
    /// A copy here would be an allocation per test, on a path every test walks.
    cc::string_view test_name;
    cc::rec::recording pending;
    bool closed = false;
    bool failed = false;
};

/// Where one thread's slicing had got to when the last block for it ended.
///
/// A chunk is dispatched in SLICES as the producer commits more of it, and the preamble naming the trace is the first
/// event of the FIRST slice only.
/// So a later slice has nothing to seed from and continues from here.
struct thread_cursor
{
    u64 chunk_seq = ~u64(0);
    u64 running = 0;
};

struct bucket_table
{
    cc::map<u64, bucket> by_trace;
    cc::map<u32, thread_cursor> by_thread;
};

/// Guards the table alone, and is NEVER held across a cc::rec call.
///
/// The listener runs under cc::rec's processing mutex and takes this one; a sync takes this one after its flush has
/// already released cc::rec's.
/// Taking them in the other order — this one first, then flushing — is the deadlock, so nothing here does.
cc::mutex<bucket_table> g_buckets;

/// Slices every chunk into ambient segments and files each one under the trace it belonged to.
struct bucketing_listener final : cc::rec::listener
{
    void on_chunk(cc::rec::chunk_view const& view) override
    {
        if (view.bytes.empty())
            return;

        auto const* segment_start = view.bytes.data();

        g_buckets.lock(
            [&](bucket_table& t)
            {
                auto& cursor = t.by_thread[view.thread.index];
                if (cursor.chunk_seq != view.chunk_seq)
                {
                    // A chunk this listener has not seen before; its preamble is the first event below and is what
                    // sets `running`, so there is nothing to seed from out of band.
                    cursor.chunk_seq = view.chunk_seq;
                    cursor.running = 0;
                }

                auto running = cursor.running;

                for (auto it = view.begin(); it != view.end(); ++it)
                {
                    auto const e = *it;

                    // The preamble states the trace outright and an ambient delta changes it — both name the context
                    // that follows them, so both cut at the same place.
                    if (e.kind() != cc::rec::event_kind::ambient_changed && e.kind() != cc::rec::event_kind::stream_state)
                        continue;

                    auto const next = e.field_as_u64("trace").value_or(0);
                    if (next == running)
                        continue;

                    // The delta belongs to the context it NAMES, so the cut goes before it — the same rule
                    // recording::from_trace follows, and the two must agree or a query and a bucket would differ.
                    file_segment(t, view, segment_start, it.position(), running);
                    running = next;
                    segment_start = it.position();
                }

                file_segment(t, view, segment_start, view.bytes.data() + view.bytes.size(), running);
                cursor.running = running;
            });
    }

    [[nodiscard]] cc::string_view listener_name() const override { return "nexus per-test"; }

private:
    static void file_segment(bucket_table& t, cc::rec::chunk_view const& view, byte const* from, byte const* to, u64 trace)
    {
        if (trace == 0 || to <= from)
            return;

        auto* const b = t.by_trace.get_ptr(trace);
        if (b == nullptr)
            return; // an unrecorded test, or work under no test at all

        auto slice = view;
        slice.bytes = cc::span<byte const>(from, to - from);
        b->pending.append(slice);
    }
};

bucketing_listener g_bucketing;

/// Everything the run recorded, kept whole for `--benchmark-rec`.
///
/// No slicing and no attribution: the question this answers is "what happened during the run", which is exactly what
/// the bucketing listener above throws away in order to answer a different one.
cc::mutex<cc::rec::recording> g_capture;

struct capture_listener final : cc::rec::listener
{
    void on_chunk(cc::rec::chunk_view const& view) override
    {
        if (view.bytes.empty())
            return;
        g_capture.lock([&](cc::rec::recording& r) { r.append(view); });
    }

    [[nodiscard]] cc::string_view listener_name() const override { return "nexus run capture"; }
};

capture_listener g_capture_listener;
cc::rec::listener_handle g_capture_handle;
cc::string g_capture_path;

/// nexus's own defaults, with `CC_LOG_*` applied over them.
///
/// Elapsed rather than wall-clock time, because a test run's question is "how far into the run" and not "what time is
/// it": the run is seconds long and its output is read next to the failure it explains.
/// The environment still wins, so `CC_LOG_LEVEL=debug uv run dev.py test "..."` reaches a test binary — which is the
/// first thing anyone chasing a library's debug output reaches for.
///
/// **Built on the first run rather than at static-initialization time.** Reading the environment and asking whether
/// stdout is a terminal are questions with no answer that early.
/// A test that pins the listener's own behavior constructs one with explicit options instead, so the suite that
/// covers this configuration is not itself configured by it.
cc::rec::console_listener& run_console()
{
    static auto listener = cc::rec::console_listener(cc::rec::console_options::from_environment({
        .min_level = cc::rec::level::info,
        .time = cc::rec::console_time::elapsed,
    }));
    return listener;
}

/// Failing tests' recordings, already serialized, waiting for a directory to be written into.
///
/// A recording holds CHUNK REFERENCES, so it cannot survive the `shutdown()` that ends a run — and a test configured
/// `owns_recorder` ends the run's recorder mid-suite, with no log directory to write to yet.
/// Serializing at that moment and keeping the BYTES is what stops a handover from destroying the evidence of every
/// test that had already failed; without it a suite containing one such test can never produce a dump.
cc::vector<cc::pair<cc::string, cc::vector<byte>>> g_pending_dumps;

cc::rec::listener_handle g_bucketing_handle;
cc::rec::listener_handle g_console_handle;
bool g_active = false;

/// Turns a test name into something that survives being a filename.
cc::string sanitized(cc::string_view name)
{
    cc::string out;
    out.reserve_back(name.size());
    for (auto const c : name)
        out += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>'
                || c == '|' || c == ' ')
                 ? '-'
                 : c;
    return out;
}
} // namespace

bool nx::impl::begin_run_recording()
{
    if (cc::rec::is_initialized())
        return false; // someone else owns it; adopting a recorder we cannot shut down would be worse than none

    cc::rec::initialize();

    // The console's min_level only filters what was recorded, and trace/debug are off at the DOMAIN by default — so
    // this is what makes `CC_LOG_LEVEL=debug uv run dev.py test "..."` show anything at all.
    cc::rec::enable_environment_log_levels();

    g_console_handle = cc::rec::register_listener(run_console());
    g_bucketing_handle = cc::rec::register_listener(g_bucketing);
    g_active = true;
    return true;
}

bool nx::impl::run_recording_active()
{
    return g_active;
}

void nx::impl::begin_run_capture(cc::string_view path)
{
    if (!g_active || path.empty())
        return;

    g_capture_path = cc::string(path);
    g_capture_handle = cc::rec::register_listener(g_capture_listener);
}

void nx::impl::end_run_recording(cc::string_view log_dir)
{
    if (!g_active)
        return;

    // One drain for the whole run, which is what deferring the per-test dumps bought.
    cc::rec::flush_blocking();

    // Scoped, and emphatically so: a recording holds CHUNK REFERENCES, and releasing one after shutdown() has deleted
    // the pool is a use-after-free with nothing between it and a corrupted heap.
    // Serializing here rather than at the write is the same constraint one step earlier — the bytes outlive the pool,
    // the recording cannot, and a handover reaches this point with nowhere to write yet.
    {
        cc::vector<cc::pair<cc::string_view, cc::rec::recording>> dumps;
        g_buckets.lock(
            [&](bucket_table& t)
            {
                for (auto&& [trace, b] : t.by_trace)
                    if (b.failed)
                        dumps.push_back({b.test_name, cc::move(b.pending)});
                t.by_trace.clear();
            });

        for (auto const& [name, r] : dumps)
            g_pending_dumps.push_back({cc::string(name), cc::rec::serialize(r)});
    }

    // The whole-run capture, under the same constraint and for the same reason: serialize while the pool is still
    // alive, and let go of the recording before shutdown() deletes the chunks it points at.
    if (!g_capture_path.empty())
    {
        auto bytes = cc::vector<byte>();
        g_capture.lock(
            [&](cc::rec::recording& r)
            {
                bytes = cc::rec::serialize(r);
                r = {};
            });

        if (auto const written = cc::rec::save_serialized_recording(bytes, g_capture_path); !written.has_value())
            cc::eprintln("nexus: could not write the run recording to `{}': {}", g_capture_path,
                         written.error().to_string());

        cc::rec::unregister_listener(g_capture_handle);
        g_capture_handle = {};
        g_capture_path = {};
    }

    if (!log_dir.empty())
    {
        for (auto const& [name, bytes] : g_pending_dumps)
        {
            auto const path = cc::format("{}/test-recording-{}.ccrec", log_dir, sanitized(name));
            if (auto const written = cc::rec::save_serialized_recording(bytes, path); !written.has_value())
                cc::eprintln("nexus: could not write the recording for `{}': {}", name, written.error().to_string());
        }
        g_pending_dumps.clear();
    }

    cc::rec::unregister_listener(g_bucketing_handle);
    cc::rec::unregister_listener(g_console_handle);
    g_bucketing_handle = {};
    g_console_handle = {};
    g_active = false;

    cc::rec::shutdown();
}

nx::impl::recorder_handover_scope::recorder_handover_scope(bool active)
{
    if (!active || !g_active)
        return;

    // Hand the singleton over whole: the test is exclusive, so nothing else is running to notice.
    end_run_recording({});
    _restore = true;
}

nx::impl::recorder_handover_scope::~recorder_handover_scope()
{
    if (_restore)
        begin_run_recording();
}

cc::rec::trace_id nx::impl::new_test_trace(bool recorded)
{
    if (!g_active || !recorded)
        return cc::rec::trace_id::none;

    return cc::rec::new_trace_id();
}

void nx::impl::open_test_bucket(cc::rec::trace_id id, cc::string_view test_name)
{
    if (!g_active || id == cc::rec::trace_id::none)
        return;

    g_buckets.lock([&](bucket_table& t) { t.by_trace[u64(id)] = bucket{.test_name = test_name}; });
}

void nx::impl::close_test_bucket(cc::rec::trace_id id, bool failed)
{
    if (!g_active || id == cc::rec::trace_id::none)
        return;

    g_buckets.lock(
        [&](bucket_table& t)
        {
            auto* const b = t.by_trace.get_ptr(u64(id));
            if (b == nullptr)
                return;

            if (!failed)
            {
                // Dropping the blocks is what releases the chunk references, and the pool cannot recycle a chunk any
                // test still holds — so a passing test letting go promptly is a memory invariant, not tidiness.
                t.by_trace.erase(u64(id));
                return;
            }

            b->closed = true;
            b->failed = true;
        });
}

cc::rec::recording nx::impl::take_test_bucket(cc::rec::trace_id id)
{
    if (!g_active || id == cc::rec::trace_id::none)
        return {};

    return g_buckets.lock(
        [&](bucket_table& t)
        {
            cc::rec::recording out;
            if (auto* const b = t.by_trace.get_ptr(u64(id)); b != nullptr)
                out = cc::move(b->pending);
            return out;
        });
}

nx::test_recorder nx::test_recording()
{
    return nx::test_recorder(cc::rec::current_trace_id());
}

cc::rec::recording nx::test_recorder::sync()
{
    if (!is_attached())
        return {};

    // Outside the bucket lock, and before taking it: the listener runs under cc::rec's mutex and takes ours, so the
    // one order this must never use is ours-then-theirs.
    cc::rec::flush_blocking();

    auto fresh = nx::impl::take_test_bucket(_trace);
    _all.append(fresh);
    return fresh;
}
