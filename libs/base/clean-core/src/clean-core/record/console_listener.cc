#include "console_listener.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>

namespace
{
cc::string_view level_tag(cc::rec::level l)
{
    switch (l)
    {
    case cc::rec::level::trace:
        return "trace";
    case cc::rec::level::debug:
        return "debug";
    case cc::rec::level::info:
        return "info ";
    case cc::rec::level::warning:
        return "warn ";
    case cc::rec::level::error:
        return "error";
    default:
        return "?????";
    }
}

cc::string colorize_level(cc::rec::level l)
{
    auto const tag = level_tag(l);
    switch (l)
    {
    case cc::rec::level::trace:
    case cc::rec::level::debug:
        return cc::console::dim(tag);
    case cc::rec::level::warning:
        return cc::console::yellow(tag);
    case cc::rec::level::error:
        return cc::console::red(tag);
    default:
        return cc::string(tag);
    }
}

/// The file name of a source location, without the directories nobody reads.
cc::string_view short_file(char const* path)
{
    auto name = cc::string_view(path == nullptr ? "" : path);
    if (auto const slash = cc::max(name.rfind('/'), name.rfind('\\')); slash >= 0)
        name = name.subview(slash + 1);
    return name;
}
} // namespace

void cc::rec::console_listener::on_chunk(cc::rec::chunk_view const& view)
{
    for (auto it = view.begin(); it != view.end(); ++it)
    {
        auto const e = *it;
        if (e.kind() != rec::event_kind::log || e.level() < _options.min_level)
            continue;

        if (!_has_origin)
        {
            _origin_secs = view.wall_secs_of(e.cycles);
            _has_origin = true;
        }

        auto line = cc::string();

        if (_options.show_time)
            line.appendf("[{:8.3f}] ", view.wall_secs_of(e.cycles) - _origin_secs);

        line += colorize_level(e.level());
        line += ' ';

        if (_options.show_thread)
        {
            if (!view.thread.name.empty())
                line.appendf("{{{}}} ", view.thread.name);
            else
                line.appendf("{{t{}}} ", view.thread.index);
        }

        if (_options.show_domain)
            line.appendf("{}: ", cc::console::dim(e.domain()->name()));

        // A message with no format arguments lives in the descriptor, so the event carries no payload at all.
        line += e.payload.empty() ? e.name() : e.payload_as_text();

        if (e.is_truncated())
            line += cc::console::dim(" ...(truncated)");

        if (_options.always_show_site || e.level() >= rec::level::warning)
            line.appendf("{}",
                         cc::console::dim(cc::format("  ({}:{})", short_file(e.site().file_name()), e.site().line())));

        _pending.push_back({
            .cycles = e.cycles,
            .text = cc::move(line),
            .to_stderr = _options.split_streams && e.level() >= rec::level::warning,
        });
    }
}

void cc::rec::console_listener::on_batch_end()
{
    if (_pending.empty())
        return;

    // Blocks arrive from different threads in no particular order, so the ordering happens here rather than as they land.
    // Only same-cycle ties are left unordered.
    cc::sort(_pending, [](pending_line const& a, pending_line const& b) { return a.cycles < b.cycles; });

    for (auto const& line : _pending)
    {
        if (line.to_stderr)
            cc::eprintln(line.text);
        else
            cc::println(line.text);
    }

    _printed += _pending.size();
    _pending.clear();
}
