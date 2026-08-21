#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>

/// Internal, not part of the public API — the job-selection policy behind the async/streaming transfer actors.
/// Kept out of the public FILE_SET on purpose; only backends and the tests reach it.
///
/// It is deliberately free of any backend type, so the whole policy — window sharing, priority, ordering — is
/// testable without a GPU, which is where nearly all of its risk lives.

namespace sg::impl
{
/// Which budget class a transfer job draws from, and whether it carries the strong scheduling guarantee.
/// `async` jobs auto-sync with command lists and run first-in-first-out; `streaming` jobs trade that for a priority.
enum class transfer_flavor : u8
{
    async,
    streaming,
};

/// One job as the scheduler sees it, rebuilt by the actor each time it picks.
/// The actor owns the real job; this is only what selection needs.
///
/// `family` is the ordering constraint: two jobs sharing one must run in `sequence` order, so same-destination
/// transfers still compose.
/// Across families the order is free, which is what lets a blocked job be filled around instead of blocking the
/// queue behind it.
struct transfer_candidate
{
    transfer_flavor flavor = transfer_flavor::async;
    i32 priority = 0;      ///< streaming only; async ignores it and runs FIFO
    float age_seconds = 0; ///< how long this job has waited, for aging
    u64 family = 0;        ///< ordering family — same destination resource
    u64 sequence = 0;      ///< submission order, globally unique and monotonic
    bool eligible = true;  ///< its waits are satisfied, its source has bytes, and it is not cancelled
};

/// Picks which job fills the open window next, and how windows are shared between async and streaming.
///
/// Window sharing is **PWM rather than splitting**: one whole window at a time goes to whichever flavor is owed
/// bandwidth, and the other flavor fills whatever that one leaves.
/// A rolling deficit is what makes that add up to the configured ratio over time, and it counts the bytes a window
/// *actually* moved — a stream-primary window that only manages a tenth of a window must not burn the whole share.
///
/// Splitting each window by ratio would look more direct and does not survive contact with textures: a placed
/// footprint has a minimum viable slice, so a reserved fraction too small for one aligned row is simply wasted.
class transfer_scheduler
{
    // configuration
public:
    /// Share of copied bytes streaming is owed over time, in [0, 1]; 0 lets async starve it completely.
    void set_stream_ratio(float ratio);

    /// Effective streaming priority is `priority + factor * age_seconds`.
    /// Defaults to 0, which means a low-priority job never runs while any higher-priority one is eligible — often
    /// exactly what a caller wants, which is why aging is opt-in rather than on.
    void set_aging_factor(float per_second);

    /// The window size the deficit is bounded against; a long idle stretch must not bank unlimited credit.
    /// Must be set before the first `on_window_submitted`, which asserts on it — an unset size silently
    /// disables the bound, which is a starvation bug rather than a missing nicety.
    void set_window_bytes(isize bytes);

    [[nodiscard]] float stream_ratio() const { return _stream_ratio; }
    [[nodiscard]] float aging_factor() const { return _aging_factor; }

    // window cycle
public:
    /// Fixes which flavor fills the window about to open.
    void begin_window();

    /// Records what the window just submitted actually moved, updating the deficit.
    void on_window_submitted(isize async_bytes, isize stream_bytes);

    [[nodiscard]] transfer_flavor window_primary() const { return _window_primary; }

    /// Bytes streaming is currently owed; negative means it ran ahead.
    /// Diagnostics and tests.
    [[nodiscard]] isize stream_deficit_bytes() const { return _stream_deficit; }

    // selection
public:
    /// The position in `candidates` of the job to pack next, or nullopt when none can make progress.
    ///
    /// Ineligible jobs are skipped rather than blocking the queue, and a family runs strictly in sequence order —
    /// so a family whose head is ineligible waits, while every other family carries on.
    /// The window's primary flavor is served first and the other one fills the leftover.
    [[nodiscard]] cc::optional<isize> pick_next(cc::span<transfer_candidate const> candidates) const;

private:
    /// Best candidate of one flavor, or nullopt; applies eligibility and the family-order rule itself.
    [[nodiscard]] cc::optional<isize> pick_of_flavor(cc::span<transfer_candidate const> candidates,
                                                     transfer_flavor flavor) const;

    float _stream_ratio = 0.1f;
    float _aging_factor = 0.0f;
    isize _window_bytes = 0;
    isize _stream_deficit = 0;
    transfer_flavor _window_primary = transfer_flavor::async;
};
} // namespace sg::impl
