#pragma once

#include <clean-core/record/fwd.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/string/string_view.hh>

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
void close_test_bucket(cc::rec::trace_id id, bool failed);

/// Takes everything bucketed for `id` since the last take.
[[nodiscard]] cc::rec::recording take_test_bucket(cc::rec::trace_id id);
} // namespace nx::impl
