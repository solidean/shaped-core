#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/string/string.hh>

// The listener that turns log events back into lines on a terminal.
//
// **Deliberately a little behind, in exchange for a total order.**
// Blocks arrive from different threads in no particular order relative to each other, so printing them as they land
// would interleave a multi-threaded run into nonsense.
// Instead one drain's worth is buffered, sorted by timestamp, and printed at the end of the batch — which leaves only
// same-cycle ties unordered.
//
// It is NOT installed for you.
// A test or example binary gets one from nx::run; an application calls install_default_console_listener.

/// Which clock a console line is stamped with, and how much of it is shown.
enum class cc::rec::console_time
{
    /// No timestamp at all.
    none,

    /// Seconds since the first event this listener saw.
    /// What a test run or a benchmark wants: the number means "how far into the run", which is the question there.
    elapsed,

    /// Local wall-clock time of day, `HH:MM:SS.mmm`.
    /// What an application wants: it lines a log up against everything else that happened on the machine, and it is
    /// what makes a stale terminal obvious at a glance.
    wall_time,

    /// Local wall-clock date and time, `YYYY-MM-DD HH:MM:SS.mmm`, for a log that outlives the day it was written.
    wall_datetime,
};

/// What a console_listener prints, and how.
///
/// **Constructing a listener FROM one of these means exactly what it says**: the environment is not consulted, so an
/// application that configured its logging cannot have that overridden out from under it.
/// A default-constructed listener resolves its options from the environment instead — see from_environment.
struct cc::rec::console_options
{
    /// Print at or above this level; everything below is skipped.
    rec::level min_level = rec::level::info;

    /// Which clock each line is stamped with.
    rec::console_time time = rec::console_time::wall_time;

    /// Prefix each line with the recording thread's name, or its index when it has none.
    bool show_thread = true;

    /// Prefix each line with the domain the site belongs to.
    bool show_domain = true;

    /// Suffix each line with the source location it was logged from.
    ///
    /// Off by default, warnings and errors included: it is a lot of noise for something a `.ccrec` already carries
    /// exactly, and offline, for every event rather than the ones that happened to be printed.
    bool show_site = false;

    /// Send warnings and errors to stderr, everything else to stdout.
    bool split_streams = true;

    /// Whether the line is colored.
    /// `automatic` is resolved once, at construction, rather than per line.
    cc::console::color_mode color = cc::console::color_mode::automatic;

    /// The options a default-constructed listener uses: these defaults, with the environment applied over them.
    ///
    /// Reading the environment is what lets someone debug a program they cannot rebuild, which is the whole reason
    /// this exists — so the variables are read once here rather than consulted per line.
    ///   CC_LOG_LEVEL   trace | debug | info | warning | error
    ///   CC_LOG_TIME    none | elapsed | time | datetime
    ///   CC_LOG_COLOR   auto | always | never   (NO_COLOR and FORCE_COLOR are honored by cc::console::resolve)
    ///   CC_LOG_THREAD  CC_LOG_DOMAIN  CC_LOG_SITE   0/false/no/off for no, anything else for yes
    ///
    /// An unset variable leaves its field alone, and an unparseable one is ignored rather than diagnosed — a
    /// misspelled log setting must never be the reason a program refuses to start.
    [[nodiscard]] static rec::console_options from_environment();
};

/// Prints log events, in timestamp order across threads.
///
/// Only `event_kind::log` reaches the terminal — a console that also printed every scope and stat would be unreadable,
/// and those have listeners of their own.
struct cc::rec::console_listener final : rec::listener
{
    /// Takes its options from the environment, over the defaults.
    console_listener() : console_listener(rec::console_options::from_environment()) {}

    /// Takes exactly these options, and never consults the environment.
    explicit console_listener(rec::console_options const& options)
      : _options(options), _colored(cc::console::resolve(options.color))
    {
    }

    void on_chunk(rec::chunk_view const& view) override;
    void on_batch_end() override;

    [[nodiscard]] cc::string_view listener_name() const override { return "console"; }

    [[nodiscard]] rec::console_options const& options() const { return _options; }

    /// Whether this listener resolved to colored output.
    [[nodiscard]] bool is_colored() const { return _colored; }

    /// How many messages have been printed, which is what a test asserts on.
    [[nodiscard]] isize printed_count() const { return _printed; }

private:
    /// One buffered line, already rendered, waiting for the batch to finish so it can be ordered.
    struct pending_line
    {
        u64 cycles = 0;
        cc::string text;
        bool to_stderr = false;
    };

    rec::console_options _options;

    /// Resolved once at construction rather than per line: `automatic` asks the OS whether a stream is a terminal,
    /// and that is not a question worth re-answering a thousand times a second.
    bool _colored = false;

    cc::vector<pending_line> _pending;
    isize _printed = 0;

    /// The wall clock of the first event ever seen, so elapsed times read as an offset into the run.
    f64 _origin_secs = 0;
    bool _has_origin = false;
};

namespace cc::rec
{
/// Gets an application's log messages onto its terminal, in one call.
///
/// Brings the recording system up if it is not up already, then registers a process-owned console_listener
/// configured from the environment.
/// An application that wants a non-default budget calls `initialize(config)` FIRST; this then only registers.
///
/// Idempotent: calling it twice registers one listener and hands back the same handle both times.
/// The listener it owns lives until the process ends, so nothing has to outlive-check it.
///
/// **A program that calls `rec::shutdown()` must unregister this first** — shutdown frees the pool every listener's
/// callback reads out of, and asserts that none are left.
/// That is what the returned handle is for; an application that simply runs until the process ends can ignore it.
///
/// **Deliberately not something a library may call.** How many megabytes recording may cost, and whether anything is
/// printed at all, are the program's decisions — see the note at the top of record/system.hh.
rec::listener_handle install_default_console_listener();
} // namespace cc::rec
