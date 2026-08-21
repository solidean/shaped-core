#include <clean-core/container/map.hh>
#include <clean-core/record/async_scope.hh>
#include <clean-core/record/console_listener.hh>
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
/// A chunk is dispatched in SLICES as the producer commits more of it, and `state_at_start` describes the start of the
/// CHUNK rather than the start of the slice.
/// So only the first slice of a chunk can be seeded from it, and every later one continues from here.
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
                    // A chunk this listener has not seen before, so its preamble is the state at this slice's start.
                    cursor.chunk_seq = view.chunk_seq;
                    cursor.running = view.state_at_start != nullptr ? view.state_at_start->trace_id : u64(0);
                }

                auto running = cursor.running;

                for (auto it = view.begin(); it != view.end(); ++it)
                {
                    auto const e = *it;
                    if (e.kind() != cc::rec::event_kind::ambient_changed)
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
cc::rec::console_listener g_console;

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

    g_console_handle = cc::rec::register_listener(g_console);
    g_bucketing_handle = cc::rec::register_listener(g_bucketing);
    g_active = true;
    return true;
}

bool nx::impl::run_recording_active()
{
    return g_active;
}

void nx::impl::end_run_recording(cc::string_view log_dir)
{
    if (!g_active)
        return;

    // One drain for the whole run, which is what deferring the per-test dumps bought.
    cc::rec::flush_blocking();

    // Scoped, and emphatically so: a recording holds CHUNK REFERENCES, and releasing one after shutdown() has deleted
    // the pool is a use-after-free with nothing between it and a corrupted heap.
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

        if (!log_dir.empty())
            for (auto const& [name, r] : dumps)
            {
                auto const path = cc::format("{}/test-recording-{}.ccrec", log_dir, sanitized(name));
                if (auto const written = cc::rec::save_recording(r, path); !written.has_value())
                    cc::eprintln("nexus: could not write the recording for `{}': {}", name, written.error().to_string());
            }
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
