#include "console_listener.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/system.hh>
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

/// Colored by level rather than by domain: the eye is looking for the bad line, and only the level says which is one.
cc::string colorize_level(cc::rec::level l, bool colored)
{
    auto const tag = level_tag(l);
    switch (l)
    {
    case cc::rec::level::trace:
    case cc::rec::level::debug:
        return cc::console::colorize(cc::console::color::dim, tag, colored);
    case cc::rec::level::warning:
        return cc::console::colorize(cc::console::color::yellow, tag, colored);
    case cc::rec::level::error:
        return cc::console::colorize(cc::console::color::red, tag, colored);
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

cc::optional<cc::rec::level> parse_level(cc::string_view s)
{
    if (s == "trace")
        return cc::rec::level::trace;
    if (s == "debug")
        return cc::rec::level::debug;
    if (s == "info")
        return cc::rec::level::info;
    if (s == "warning" || s == "warn")
        return cc::rec::level::warning;
    if (s == "error")
        return cc::rec::level::error;
    return {};
}

cc::optional<cc::rec::console_time> parse_time(cc::string_view s)
{
    if (s == "none" || s == "off")
        return cc::rec::console_time::none;
    if (s == "elapsed")
        return cc::rec::console_time::elapsed;
    if (s == "time")
        return cc::rec::console_time::wall_time;
    if (s == "datetime")
        return cc::rec::console_time::wall_datetime;
    return {};
}

cc::optional<cc::console::color_mode> parse_color(cc::string_view s)
{
    if (s == "auto" || s == "automatic")
        return cc::console::color_mode::automatic;
    if (s == "always" || s == "1" || s == "yes")
        return cc::console::color_mode::always;
    if (s == "never" || s == "0" || s == "no")
        return cc::console::color_mode::never;
    return {};
}

/// Applies one flag variable, leaving the field alone when it is unset.
void apply_flag(cc::string_view name, bool& field)
{
    if (cc::environment_variable(name).has_value())
        field = cc::is_environment_flag_set(name);
}
} // namespace

cc::rec::console_options cc::rec::console_options::from_environment()
{
    auto options = rec::console_options{};

    if (auto const v = cc::environment_variable("CC_LOG_LEVEL"); v.has_value())
        if (auto const parsed = parse_level(v.value()); parsed.has_value())
            options.min_level = parsed.value();

    if (auto const v = cc::environment_variable("CC_LOG_TIME"); v.has_value())
        if (auto const parsed = parse_time(v.value()); parsed.has_value())
            options.time = parsed.value();

    if (auto const v = cc::environment_variable("CC_LOG_COLOR"); v.has_value())
        if (auto const parsed = parse_color(v.value()); parsed.has_value())
            options.color = parsed.value();

    apply_flag("CC_LOG_THREAD", options.show_thread);
    apply_flag("CC_LOG_DOMAIN", options.show_domain);
    apply_flag("CC_LOG_SITE", options.show_site);

    return options;
}

void cc::rec::console_listener::on_chunk(cc::rec::chunk_view const& view)
{
    for (auto it = view.begin(); it != view.end(); ++it)
    {
        auto const e = *it;
        if (e.kind() != rec::event_kind::log || e.level() < _options.min_level)
            continue;

        auto const wall_secs = view.wall_secs_of(e.cycles);
        if (!_has_origin)
        {
            _origin_secs = wall_secs;
            _has_origin = true;
        }

        auto line = cc::string();

        switch (_options.time)
        {
        case rec::console_time::none:
            break;

        case rec::console_time::elapsed:
            line.appendf("[{:8.3f}] ", wall_secs - _origin_secs);
            break;

        case rec::console_time::wall_time:
        {
            auto const t = cc::local_calendar_time(wall_secs);
            line.appendf("[{:02}:{:02}:{:02}.{:03}] ", t.hour, t.minute, t.second, t.millisecond);
            break;
        }

        case rec::console_time::wall_datetime:
        {
            auto const t = cc::local_calendar_time(wall_secs);
            line.appendf("[{}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] ", t.year, t.month, t.day, t.hour, t.minute,
                         t.second, t.millisecond);
            break;
        }
        }

        line += colorize_level(e.level(), _colored);
        line += ' ';

        if (_options.show_thread)
        {
            if (!view.thread.name.empty())
                line.appendf("{{{}}} ", view.thread.name);
            else
                line.appendf("{{t{}}} ", view.thread.index);
        }

        if (_options.show_domain)
            line.appendf("{}: ", cc::console::colorize(cc::console::color::dim, e.domain()->name(), _colored));

        // A message with no format arguments lives in the descriptor, so the event carries no payload at all.
        line += e.payload.empty() ? e.name() : e.payload_as_text();

        if (e.is_truncated())
            line += cc::console::colorize(cc::console::color::dim, " ...(truncated)", _colored);

        if (_options.show_site)
        {
            auto const site = cc::format("  ({}:{})", short_file(e.site().file), e.site().line);
            line += cc::console::colorize(cc::console::color::dim, site, _colored);
        }

        // The newline is part of the line, so printing it is ONE write.
        // cc::println issues the text and the newline separately and is explicitly not atomic, which interleaves
        // two threads' lines into each other whenever both are printing.
        line += '\n';

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
            cc::eprint(line.text);
        else
            cc::print(line.text);
    }

    // print() does not flush, so the batch is handed over in one go at the end rather than per line.
    cc::flush();
    cc::eflush();

    _printed += _pending.size();
    _pending.clear();
}

cc::rec::listener_handle cc::rec::install_default_console_listener()
{
    // One function-local static does the whole thing exactly once, initialization order included.
    //
    // Function-local rather than file-scope so it is built on the first CALL: reading the environment and asking
    // whether stdout is a terminal are questions with no answer during static initialization.
    // Never destroyed — unregistering at exit would race every thread still recording, and the process is ending.
    static auto const handle = []
    {
        if (!rec::is_initialized())
            rec::initialize();

        return rec::register_listener(*new rec::console_listener());
    }();

    return handle;
}
