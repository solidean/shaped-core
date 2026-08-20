#include "chrome_trace.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

using namespace cc::primitive_defines;

namespace
{
using namespace cc::primitive_defines;

/// One event plus the block it came from, since the timestamp mapping is per block.
struct located_event
{
    cc::rec::chunk_view view;
    cc::rec::event_view event;
};

/// Appends `s` as a JSON string literal, quotes included.
void append_json_string(cc::string& out, cc::string_view s)
{
    out += '"';
    for (auto const c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            // Everything below 0x20 must be escaped; UTF-8 continuation bytes pass through untouched, which is what
            // keeps a non-ASCII name readable rather than mangled into \u sequences.
            if (static_cast<unsigned char>(c) < 0x20)
                out.appendf("\\u{:04x}", u32(static_cast<unsigned char>(c)));
            else
                out += c;
            break;
        }
    }
    out += '"';
}

/// Renders a double the way JSON needs it, with no infinities or NaNs to trip a parser.
void append_json_number(cc::string& out, f64 v)
{
    if (v != v || v > 1e308 || v < -1e308)
    {
        out += "null";
        return;
    }
    out.appendf("{}", v);
}

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
} // namespace

cc::result<cc::vector<byte>> babel::chrome_trace::encode(cc::rec::recording const& recording, write_options opts)
{
    // The mapping from cycles to time is per block, so every event travels with the block it came from.
    cc::vector<located_event> events;
    recording.for_each_event([&](cc::rec::chunk_view const& v, cc::rec::event_view const& e)
                             { events.push_back({v, e}); });

    cc::sort(events, [](located_event const& a, located_event const& b) { return a.event.cycles < b.event.cycles; });

    // Relative to the earliest event rather than to the epoch: absolute time would spend most of a double's precision
    // on a number no viewer shows.
    f64 origin_secs = 0;
    for (auto const& le : events)
    {
        auto const secs = le.view.wall_secs_of(le.event.cycles);
        if (origin_secs == 0 || secs < origin_secs)
            origin_secs = secs;
    }

    auto out = cc::string::create_with_capacity(events.size() * 128 + 256);
    auto const separator = opts.pretty ? cc::string_view(",\n ") : cc::string_view(",");

    out += opts.pretty ? "{\n\"traceEvents\": [\n " : "{\"traceEvents\":[";

    auto first = true;
    auto const open_event = [&]
    {
        if (!first)
            out += separator;
        first = false;
        out += '{';
    };

    // Process and thread names, so the viewer shows something better than a bare id.
    {
        open_event();
        out.appendf(R"("ph":"M","name":"process_name","pid":{},"tid":0,"args":{{"name":)", opts.process_id);
        append_json_string(out, opts.process_name);
        out += "}}";
    }

    cc::set<u32> named_threads;
    for (auto const& le : events)
    {
        auto const tid = le.view.thread.index;
        if (named_threads.contains(tid))
            continue;
        named_threads.insert(tid);

        open_event();
        out.appendf(R"("ph":"M","name":"thread_name","pid":{},"tid":{},"args":{{"name":)", opts.process_id, tid);
        append_json_string(out, le.view.thread.name.empty() ? cc::string_view("thread") : le.view.thread.name);
        out += "}}";
    }

    // An accumulate carries a delta, but a counter track shows a level — so the running total is what gets emitted.
    cc::map<cc::string, f64> running_totals;

    for (auto const& le : events)
    {
        auto const& e = le.event;
        auto const kind = e.kind();

        if (is_system_event(e) && !opts.include_system_events)
            continue;

        auto const ts_us = (le.view.wall_secs_of(e.cycles) - origin_secs) * 1e6;
        auto const tid = le.view.thread.index;

        auto const common = [&](cc::string_view phase)
        {
            open_event();
            out.appendf(R"("ph":"{}","pid":{},"tid":{},"ts":{:.3f},"name":)", phase, opts.process_id, tid, ts_us);
            append_json_string(out, kind == cc::rec::event_kind::log ? log_text(e) : e.name());
            out += R"(,"cat":)";
            append_json_string(out, e.domain()->name());
        };

        switch (kind)
        {
        case cc::rec::event_kind::scope_begin:
            if (!opts.include_scopes)
                continue;
            common("B");
            out += '}';
            break;

        case cc::rec::event_kind::scope_end:
            if (!opts.include_scopes)
                continue;
            // "E" carries no name in the format, but writing one keeps a hand-read trace legible and viewers ignore it.
            common("E");
            out += '}';
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

            common("C");
            out += R"(,"args":{"value":)";
            append_json_number(out, level);
            out += "}}";
            break;
        }

        case cc::rec::event_kind::log:
        {
            if (!opts.include_logs)
                continue;

            common("i");
            out += R"(,"s":"t","args":{"level":)";
            append_json_string(out, level_name(e.level()));
            out += R"(,"site":)";
            append_json_string(out, cc::format("{}:{}", e.site().file, e.site().line));
            out += "}}";
            break;
        }

        default:
        {
            if ((kind == cc::rec::event_kind::value || kind == cc::rec::event_kind::marker) && !opts.include_values)
                continue;

            common("i");
            out += R"(,"s":"t")";

            // Whatever the payload declares, rendered generically — a viewer shows it without the exporter having
            // heard of the type.
            if (auto const fields = e.fields(); !fields.empty())
            {
                out += R"(,"args":{)";
                auto first_field = true;
                for (auto const& f : fields)
                {
                    if (!first_field)
                        out += ',';
                    first_field = false;

                    append_json_string(out, f.name);
                    out += ':';

                    if (auto const t = e.field_as_text(f.name); t.has_value())
                        append_json_string(out, t.value());
                    else if (auto const d = e.field_as_double(f.name); d.has_value())
                        append_json_number(out, d.value());
                    else
                        out += "null";
                }
                out += '}';
            }
            out += '}';
            break;
        }
        }
    }

    out += opts.pretty ? "\n],\n\"displayTimeUnit\": \"ms\"\n}\n" : R"(],"displayTimeUnit":"ms"})";

    auto bytes = cc::vector<byte>();
    bytes.resize_to_uninitialized(out.size());
    if (!out.empty())
        cc::memcpy(bytes.data(), out.data(), size_t(out.size()));
    return bytes;
}

cc::result<cc::unit> babel::chrome_trace::write(cc::write_stream& out,
                                                cc::rec::recording const& recording,
                                                write_options opts)
{
    auto encoded = encode(recording, opts);
    CC_RETURN_IF_ERROR(encoded);

    out.write(cc::span<byte const>(encoded.value()));
    return cc::unit{};
}
