#include "chrome_trace.hh"

#include <babel-serializer/data/json.hh>
#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/platform/symbolize.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/streams/growing_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

using namespace cc::primitive_defines;

namespace
{
using namespace cc::primitive_defines;

/// One event plus the block it came from, since the timestamp mapping is per block.
/// One sample, reduced to what a flame graph needs.
struct sampled_stack
{
    f64 ts_us = 0;

    /// Innermost first, as cc::capture_stack produces them.
    cc::vector<u64> frames;
};

struct located_event
{
    cc::rec::chunk_view view;
    cc::rec::event_view event;
};

cc::string_view level_name(cc::rec::level l)
{
    switch (l)
    {
    case cc::rec::level::trace:
        return "trace";
    case cc::rec::level::debug:
        return "debug";
    case cc::rec::level::info:
        return "info";
    case cc::rec::level::warning:
        return "warning";
    case cc::rec::level::error:
        return "error";
    default:
        return "unknown";
    }
}

/// Whether an event belongs to the recorder's own accounting rather than to the program being recorded.
bool is_system_event(cc::rec::event_view const& e)
{
    switch (e.kind())
    {
    case cc::rec::event_kind::gap:
    case cc::rec::event_kind::chunk_acquired:
    case cc::rec::event_kind::late_event:
    case cc::rec::event_kind::dropped_span:
        return true;
    default:
        return false;
    }
}

/// What a log event should print as: the formatted payload, or the descriptor's text when the site took no arguments.
cc::string_view log_text(cc::rec::event_view const& e)
{
    return e.payload.empty() ? e.name() : e.payload_as_text();
}

/// Whether this event knows where it was recorded, which every descriptor carries.
bool has_source(cc::rec::event_view const& e)
{
    return e.desc != nullptr && e.desc->site.file != nullptr && e.desc->site.file[0] != char(0);
}

/// The `"source": "path:line"` pair.
///
/// In args rather than in the name, for the same reason a sampled frame's location is: a viewer groups spans BY
/// name, so a name carrying a line number would split one scope into a span per line.
/// The path is whole rather than a file name, because a reader following a profile wants to open the file, and
/// two `renderer.cc` in different directories are otherwise indistinguishable.
void write_source(babel::json::object_writer& args, cc::rec::event_view const& e)
{
    args.write("source", cc::format("{}:{}", e.desc->site.file, e.desc->site.line));
}

/// Writes every event of the recording into the already-open `traceEvents` array.
void write_events(babel::json::array_writer& events,
                  cc::rec::recording const& recording,
                  babel::chrome_trace::write_options const& opts)
{
    namespace json = babel::json;

    // Pretty output is one event per LINE rather than one field per line: a trace has too many events for the latter
    // to be readable, and a line per event is what makes it greppable and diffable.
    auto const event_layout = opts.pretty ? json::layout::compact : json::layout::inherit;

    // The mapping from cycles to time is per block, so every event travels with the block it came from.
    cc::vector<located_event> located;
    recording.for_each_event([&](cc::rec::chunk_view const& v, cc::rec::event_view const& e)
                             { located.push_back({v, e}); });

    cc::sort(located, [](located_event const& a, located_event const& b) { return a.event.cycles < b.event.cycles; });

    // Relative to the earliest event rather than to the epoch: absolute time would spend most of a double's precision
    // on a number no viewer shows.
    f64 origin_secs = 0;
    for (auto const& le : located)
    {
        auto const secs = le.view.wall_secs_of(le.event.cycles);
        if (origin_secs == 0 || secs < origin_secs)
            origin_secs = secs;
    }

    // Process and thread names, so the viewer shows something better than a bare id.
    {
        auto e = events.write_object(event_layout);
        e.write("ph", "M");
        e.write("name", "process_name");
        e.write("pid", opts.process_id);
        e.write("tid", 0);
        auto args = e.write_object("args");
        args.write("name", opts.process_name);
    }

    cc::set<u32> named_threads;
    for (auto const& le : located)
    {
        auto const tid = le.view.thread.index;
        if (named_threads.contains(tid))
            continue;
        named_threads.insert(tid);

        auto e = events.write_object(event_layout);
        e.write("ph", "M");
        e.write("name", "thread_name");
        e.write("pid", opts.process_id);
        e.write("tid", tid);
        auto args = e.write_object("args");
        args.write("name", le.view.thread.name.empty() ? cc::string_view("thread") : le.view.thread.name);
    }

    // An accumulate carries a delta, but a counter track shows a level — so the running total is what gets emitted.
    cc::map<cc::string, f64> running_totals;

    // One symbolizer for the whole export, because the cache is what makes this affordable: a sampled profile is
    // thousands of hits on a handful of addresses, and each miss is a debug-info lookup.
    //
    // Against the recording's OWN module table when it has one, which a loaded recording does — its addresses mean
    // the process that recorded them, not this one, and resolving them against this process would be confident
    // nonsense rather than an error.
    // One construction rather than a ternary: an empty module span already MEANS "this process's own", and a ternary
    // would need cc::symbolizer to be copyable or movable, which it deliberately is not.
    auto symbols = cc::symbolizer(recording.modules());

    // Sampled frames are emitted INSIDE the scopes that were open, on the same track.
    //
    // That is the whole point of the combination: the scopes give the structure a human named, and the samples give
    // what was happening inside them — so a viewer shows instrumentation depth with sampling stacked on top.
    // Chrome builds one stack per tid from the time-ordered B/E, so the only rule is that a synthetic span opens and
    // closes strictly between the boundaries of the scope it sits in.
    cc::map<i32, cc::vector<u64>> open_sampled;

    /// Ends every synthetic span on `tid`, deepest first.
    /// Called before any real scope boundary, so a sampled span never straddles one.
    auto const close_sampled = [&](i32 tid, f64 ts)
    {
        auto* const open = open_sampled.get_ptr(tid);
        if (open == nullptr || open->empty())
            return;

        for (isize i = open->size() - 1; i >= 0; --i)
        {
            auto e = events.write_object(event_layout);
            e.write("ph", "E");
            e.write("pid", opts.process_id);
            e.write("tid", tid);
            e.write("ts", ts, cc::float_notation::fixed, 3);
        }
        open->clear();
    };

    /// Extends the spans this sample shares with the previous one, and opens the rest.
    auto const apply_sample = [&](i32 tid, f64 ts, cc::span<u64 const> innermost_first)
    {
        auto& open = open_sampled[tid];

        // A capture runs innermost first; a flame graph is drawn outermost first.
        cc::vector<u64> stack;
        stack.reserve(innermost_first.size());
        for (isize i = innermost_first.size() - 1; i >= 0; --i)
            stack.push_back(innermost_first[i]);

        isize shared = 0;
        while (shared < open.size() && shared < stack.size() && open[shared] == stack[shared])
            ++shared;

        for (isize i = open.size() - 1; i >= shared; --i)
        {
            auto e = events.write_object(event_layout);
            e.write("ph", "E");
            e.write("pid", opts.process_id);
            e.write("tid", tid);
            e.write("ts", ts, cc::float_notation::fixed, 3);
        }

        for (isize i = shared; i < stack.size(); ++i)
        {
            auto const& info
                = opts.symbolize_samples ? symbols.resolve(reinterpret_cast<void const*>(stack[i])) : cc::symbol_info{};

            auto e = events.write_object(event_layout);
            e.write("ph", "B");
            e.write("pid", opts.process_id);
            e.write("tid", tid);
            e.write("ts", ts, cc::float_notation::fixed, 3);

            // The address stays the name when nothing resolved — a hex frame a reader can look up beats a confident
            // wrong one, and it is what a recording from another process gets.
            if (info.has_function())
                e.write("name", info.function);
            else
                e.write("name", cc::format("0x{:x}", stack[i]));

            e.write("cat", "sampled");

            // The source location goes in args rather than the name: a viewer groups spans BY name, and a name
            // carrying a line number would split one function into a span per line.
            auto args = e.write_object("args");
            args.write("address", cc::format("0x{:x}", stack[i]));
            if (info.has_line())
                args.write("source", cc::format("{}:{}", info.file, info.line));
            if (!info.module.empty())
                args.write("module", info.module);
        }

        open = cc::move(stack);
    };

    cc::set<i32> named_sample_tracks;
    auto last_ts_us = 0.0;

    for (auto const& le : located)
    {
        auto const& e = le.event;
        auto const kind = e.kind();

        if (is_system_event(e) && !opts.include_system_events)
            continue;

        auto const ts_us = (le.view.wall_secs_of(e.cycles) - origin_secs) * 1e6;
        auto const tid = le.view.thread.index;

        /// Opens an event object and writes the five fields every non-metadata event carries.
        /// The scope is returned still open, so the caller adds what its own kind needs.
        auto const common = [&](cc::string_view phase)
        {
            auto ev = events.write_object(event_layout);
            ev.write("ph", phase);
            ev.write("pid", opts.process_id);
            ev.write("tid", tid);
            ev.write("ts", ts_us, cc::float_notation::fixed, 3);
            ev.write("name", kind == cc::rec::event_kind::log ? log_text(e) : e.name());
            ev.write("cat", e.domain()->name());
            return ev;
        };

        last_ts_us = ts_us;

        if (kind == cc::rec::event_kind::sample)
        {
            if (!opts.include_samples)
                continue;

            // The payload names the sampled thread, which is not this block's thread until the recording has been
            // spliced — so reading it here works either way.
            auto const owner = u32(e.field_as_u64("thread_index").value_or(u64(tid)));

            // A thread the recorder never knew has no track of its own to nest into, so it gets one, named by the id
            // the OS knows it as.
            auto const sample_tid = owner == cc::rec::impl::sample_unknown_thread
                                      ? opts.sampled_tid_offset + i32(e.field_as_u64("native_tid").value_or(0))
                                      : i32(owner);

            if (owner == cc::rec::impl::sample_unknown_thread && !named_sample_tracks.contains(sample_tid))
            {
                named_sample_tracks.insert(sample_tid);

                auto ev = events.write_object(event_layout);
                ev.write("ph", "M");
                ev.write("name", "thread_name");
                ev.write("pid", opts.process_id);
                ev.write("tid", sample_tid);
                auto args = ev.write_object("args");
                args.write("name", cc::format("os thread {} (sampled)", e.field_as_u64("native_tid").value_or(0)));
            }

            apply_sample(sample_tid, ts_us, e.field_as_u64_array("frames"));
            continue;
        }

        // A scope boundary is where a synthetic span must not straddle, so everything sampled closes first.
        if (kind == cc::rec::event_kind::scope_begin || kind == cc::rec::event_kind::scope_end)
            close_sampled(i32(tid), ts_us);

        switch (kind)
        {
        case cc::rec::event_kind::scope_begin:
        {
            if (!opts.include_scopes)
                continue;

            auto ev = common("B");
            if (has_source(e))
            {
                auto args = ev.write_object("args");
                write_source(args, e);
            }
            break;
        }

        case cc::rec::event_kind::scope_end:
            // "E" carries no name in the format, but writing one keeps a hand-read trace legible and viewers ignore it.
            if (opts.include_scopes)
                common("E");
            break;

        case cc::rec::event_kind::stat_snapshot:
        case cc::rec::event_kind::stat_accumulate:
        {
            if (!opts.include_stats)
                continue;

            auto const value = e.field_as_double("value").value_or(0);
            auto level = value;
            if (kind == cc::rec::event_kind::stat_accumulate)
            {
                auto& total = running_totals[cc::string(e.name())];
                total += value;
                level = total;
            }

            auto ev = common("C");
            auto args = ev.write_object("args");
            args.write("value", level);
            break;
        }

        case cc::rec::event_kind::log:
        {
            if (!opts.include_logs)
                continue;

            auto ev = common("i");
            ev.write("s", "t");
            auto args = ev.write_object("args");
            args.write("level", level_name(e.level()));
            write_source(args, e); // one key across every kind, so a reader learns it once
            break;
        }

        default:
        {
            if ((kind == cc::rec::event_kind::value || kind == cc::rec::event_kind::marker) && !opts.include_values)
                continue;

            auto ev = common("i");
            ev.write("s", "t");

            // A relation says what it MEANS as well as who it links, so the edge is readable without the viewer
            // knowing the vocabulary.
            if (auto const* const t = e.relation(); t != nullptr)
            {
                ev.write("relation", t->name);
                if (t->is_equivalence)
                    ev.write("equivalence", true);
            }

            // Whatever the payload declares, rendered generically — a viewer shows it without the exporter having
            // heard of the type.
            // A counter's args are its data series, so a string among them would corrupt the plot — which is why
            // the source goes on instants and scopes and deliberately not on stat events.
            if (auto const fields = e.fields(); !fields.empty() || has_source(e))
            {
                auto args = ev.write_object("args");
                if (has_source(e))
                    write_source(args, e);

                for (auto const& f : fields)
                {
                    if (auto const t = e.field_as_text(f.name); t.has_value())
                        args.write(f.name, t.value());
                    else if (f.type == cc::rec::type_code::u64_array)
                    {
                        auto values = args.write_array(f.name);
                        for (auto const v : e.field_as_u64_array(f.name))
                            values.write(v);
                    }
                    else if (f.type == cc::rec::type_code::u64_)
                        args.write(f.name, e.field_as_u64(f.name).value_or(0));
                    else if (auto const d = e.field_as_double(f.name); d.has_value())
                        args.write(f.name, d.value());
                    else
                        args.write(f.name, nullptr);
                }
            }
            break;
        }
        }
    }

    // Whatever is still open at the end closes at the last event's time.
    for (auto const& [tid, open] : open_sampled)
        if (!open.empty())
            close_sampled(tid, last_ts_us);
}
} // namespace

cc::result<cc::unit> babel::chrome_trace::write(cc::write_stream& out,
                                                cc::rec::recording const& recording,
                                                write_options opts)
{
    // non_finite -> null: a NaN or an infinity in a recorded value must not take the whole trace down, and a viewer
    //   renders a null datapoint as a hole in the plot, which is what it is.
    // large_integers -> string: a recorded u64 is usually an id or an address rather than a quantity, so precision is
    //   all it has, and every viewer here parses numbers into a double.
    auto w = json::writer(out, {
                                   .indent = opts.pretty ? 1 : 0,
                                   .non_finite = json::non_finite_policy::null,
                                   .large_integers = json::large_integer_policy::string,
                               });

    {
        auto root = w.object();
        {
            auto events = root.write_array("traceEvents");
            write_events(events, recording, opts);
        }
        root.write("displayTimeUnit", "ms");
    }

    return w.finish();
}

cc::result<cc::vector<byte>> babel::chrome_trace::encode(cc::rec::recording const& recording, write_options opts)
{
    auto sink = cc::vector_write_stream_adapter();
    cc::write_stream stream = sink;
    CC_RETURN_IF_ERROR(write(stream, recording, opts));
    return sink.take();
}
