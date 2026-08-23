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

using namespace cc::primitive_defines;

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

/// The bracketed stamp a line opens with, empty for console_time::none.
///
/// `origin_secs` is only read by the elapsed mode, and is the earliest event the listener has printed rather than the
/// first one it saw — which is why this runs after the batch is sorted rather than as each event lands.
cc::string timestamp_of(cc::rec::console_time mode, f64 wall_secs, f64 origin_secs)
{
    auto out = cc::string();
    switch (mode)
    {
    case cc::rec::console_time::none:
        break;

    case cc::rec::console_time::elapsed:
        out.appendf("[{:8.3f}] ", wall_secs - origin_secs);
        break;

    case cc::rec::console_time::wall_time:
    {
        auto const t = cc::local_calendar_time(wall_secs);
        out.appendf("[{:02}:{:02}:{:02}.{:03}] ", t.hour, t.minute, t.second, t.millisecond);
        break;
    }

    case cc::rec::console_time::wall_datetime:
    {
        auto const t = cc::local_calendar_time(wall_secs);
        out.appendf("[{}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] ", t.year, t.month, t.day, t.hour, t.minute, t.second,
                    t.millisecond);
        break;
    }
    }
    return out;
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
    return from_environment(rec::console_options{});
}

cc::rec::console_options cc::rec::console_options::from_environment(rec::console_options base)
{
    auto options = base;

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

        // The timestamp is NOT rendered here — see pending_line.
        auto line = cc::string();

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
            .wall_secs = wall_secs,
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

    // After the sort, so the run's zero is its EARLIEST event rather than whichever chunk this listener saw first.
    if (!_has_origin)
    {
        _origin_secs = _pending.front().wall_secs;
        _has_origin = true;
    }

    for (auto const& line : _pending)
    {
        auto stamped = timestamp_of(_options.time, line.wall_secs, _origin_secs);
        stamped += line.text;

        if (line.to_stderr)
            cc::eprint(stamped);
        else
            cc::print(stamped);
    }

    // print() does not flush, so the batch is handed over in one go at the end rather than per line.
    cc::flush();
    cc::eflush();

    _printed += _pending.size();
    _pending.clear();
}

void cc::rec::enable_environment_log_levels()
{
    auto const v = cc::environment_variable("CC_LOG_LEVEL");
    if (!v.has_value())
        return;

    auto const parsed = parse_level(v.value());
    if (!parsed.has_value())
        return;

    // Every level AT OR ABOVE the named one, OR-ed onto what each domain already has.
    auto bits = u32(0);
    for (auto const l : {rec::level::trace, rec::level::debug, rec::level::info, rec::level::warning, rec::level::error})
        if (l >= parsed.value())
            bits |= rec::enable_bit_of(l);

    // The ADDITIVE form, and the distinction has teeth: set_all_domains_enabled_mask is absolute, so calling it here
    // would turn back OFF every level the variable does not name — including one a program enabled in code.
    // It covers domains registered later too, which is what reaches a plugin loaded after main() begins.
    rec::enable_all_domains_mask_bits(bits);
}

cc::rec::listener_handle cc::rec::install_default_console_listener()
{
    // Function-local rather than file-scope so the listener is built on the first CALL: reading the environment and
    // asking whether stdout is a terminal are questions with no answer during static initialization.
    // Never destroyed — unregistering at exit would race every thread still recording, and the process is ending.
    static auto* const listener = new rec::console_listener();

    // The HANDLE is deliberately not cached alongside it.
    // A program that unregisters this before rec::shutdown() — which the docs require of anyone calling shutdown at
    // all — would otherwise be handed a dead handle by the next call, when re-registering is exactly what it asked for.
    static auto handle = rec::listener_handle{};

    if (!rec::is_initialized())
        rec::initialize();

    // Here rather than in the listener's constructor: opening a domain's gate is a process-wide act, and a listener
    // is not the thing that gets to make it.
    rec::enable_environment_log_levels();

    if (!rec::is_listener_registered(handle))
        handle = rec::register_listener(*listener);

    return handle;
}
