#pragma once

#include <clean-core/record/fwd.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/fwd.hh>

// The run's side of per-test recording: standing the recorder up, attributing each test, and bucketing what lands.
//
// One listener for the whole run, not one per test.
// Every listener callback runs under cc::rec's single processing mutex, so N in-flight tests with a listener each
// would be N full scans of every chunk, serialized — the cost landing on the tests that record the MOST rather than
// on the ones asking questions.
//
// The single listener scans once and slices.
// Attribution only changes where an `ambient_changed` delta says it does, so between two deltas the listener extends
// one byte range instead of looking at events at all, and a chunk yields one block per ambient SEGMENT rather than one
// per event.
// A segment whose trace has no bucket — an unrecorded test, or work outside any test — costs one lookup and is dropped.

namespace nx::impl
{
/// Stands the recorder up for a whole run, with the console listener and the bucketing listener installed.
/// A no-op, reporting false, if a recorder is already initialized — a nested nx::run must not adopt one it cannot own.
bool begin_run_recording();

/// Tears it back down, writing out whatever failing tests left behind.
/// `log_dir` is where those dumps go; nothing is written when it is empty or no test failed.
void end_run_recording(cc::string_view log_dir);

/// Whether the run stood a recorder up.
[[nodiscard]] bool run_recording_active();

/// Keep EVERY event of this run, and write it to `path` when the run ends.
///
/// Distinct from the per-test bucketing beside it, and deliberately so: a bucket is attributed to one test and a
/// passing test's bucket is dropped, where this keeps the whole stream — warmup, scheduling, and whatever the code
/// under test recorded along the way.
///
/// **Call it after begin_run_recording and before anything runs.** Events already drained are gone.
/// Writing happens inside end_run_recording, because a recording holds chunk references and cannot outlive the
/// shutdown that ends the run — which is the same constraint the failing-test dumps are serialized early for.
void begin_run_capture(cc::string_view path);

/// Hands the recorder over to a test that drives cc::rec::initialize itself, and takes it back afterwards.
/// Only legal for an exclusive test: a torn-down recorder is torn down for every thread at once.
struct recorder_handover_scope
{
    explicit recorder_handover_scope(bool active);
    ~recorder_handover_scope();

    recorder_handover_scope(recorder_handover_scope const&) = delete;
    recorder_handover_scope& operator=(recorder_handover_scope const&) = delete;

private:
    bool _restore = false;
};

/// Mints the trace id one test is attributed under.
/// Returns `none` when the run is not bucketing, or when the test asked not to be — in which case the caller installs
/// no link and the test's events fall into no bucket at all.
[[nodiscard]] cc::rec::trace_id new_test_trace(bool recorded);

/// Opens a bucket for `id`, so the run's listener starts keeping that test's events.
void open_test_bucket(cc::rec::trace_id id, cc::string_view test_name);

/// Closes it again.
///
/// A passing test drops its events here, which is what keeps the chunk pool from filling with history nobody wants.
/// A failing one is kept until the end of the run and written out there — deferred rather than flushed per test,
/// because a flush per test is a process-wide drain thousands of times over, and a dump is read after the run anyway.
///
/// `await_log_verdict` keeps a passing test's events too, undecided, because the log rule may still fail it; see
/// settle_test_bucket.
void close_test_bucket(cc::rec::trace_id id, bool failed, bool await_log_verdict);

/// Decides a bucket closed undecided, once the log rule has judged its test.
void settle_test_bucket(cc::rec::trace_id id, bool failed);

/// Takes everything bucketed for `id` since the last take.
[[nodiscard]] cc::rec::recording take_test_bucket(cc::rec::trace_id id);

/// A warning or error the run kept for the log rule, copied out of its chunk so it outlives the recorder.
struct kept_log_record
{
    cc::rec::level level = {};
    cc::string_view domain; ///< a domain is a static object, so its name outlives the run
    cc::string text;
    char const* file = nullptr; ///< a site is static too
    u32 line = 0;
};

/// Mints the owner id one section pass runs under, or `none` when the run is not recording.
[[nodiscard]] cc::rec::trace_id new_pass_owner();

/// Takes the records kept under `owner`.
[[nodiscard]] cc::vector<kept_log_record> take_log_records(u64 owner);

/// Whether any record is kept under one of `owners` yet.
/// Only what the recorder has already delivered, so a record still in a thread's buffer answers false.
[[nodiscard]] bool has_log_records(cc::span<u64 const> owners);

/// Takes the records kept under no owner at all.
[[nodiscard]] cc::vector<kept_log_record> take_unattributed_log_records();

/// Drops every record still kept, for owners nobody judged.
void discard_log_records();

/// Writes the records still kept to stderr, for a crash handler.
/// Best effort: skipped, and said so, when the store is locked, since a crash handler must not wait.
void report_withheld_log_records() noexcept;
} // namespace nx::impl
