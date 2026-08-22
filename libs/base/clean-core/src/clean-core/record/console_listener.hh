#pragma once

#include <clean-core/container/vector.hh>
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
// A test or example binary gets one from nx::run; an application installs its own.

/// What a console_listener prints, and how.
struct cc::rec::console_options
{
    /// Print at or above this level; everything below is skipped.
    rec::level min_level = rec::level::info;

    /// Prefix each line with seconds since the first event it saw.
    bool show_time = true;

    /// Prefix each line with the recording thread's name, or its index when it has none.
    bool show_thread = true;

    /// Prefix each line with the domain the site belongs to.
    bool show_domain = true;

    /// Print the source location of every message, not just of warnings and errors.
    bool always_show_site = false;

    /// Send warnings and errors to stderr, everything else to stdout.
    bool split_streams = true;
};

/// Prints log events, in timestamp order across threads.
///
/// Only `event_kind::log` reaches the terminal — a console that also printed every scope and stat would be unreadable,
/// and those have listeners of their own.
struct cc::rec::console_listener final : rec::listener
{
    console_listener() = default;
    explicit console_listener(rec::console_options const& options) : _options(options) {}

    void on_chunk(rec::chunk_view const& view) override;
    void on_batch_end() override;

    [[nodiscard]] cc::string_view listener_name() const override { return "console"; }

    [[nodiscard]] rec::console_options const& options() const { return _options; }

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
    cc::vector<pending_line> _pending;
    isize _printed = 0;

    /// The wall clock of the first event ever seen, so times read as an offset into the run.
    f64 _origin_secs = 0;
    bool _has_origin = false;
};
