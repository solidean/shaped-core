#pragma once

#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>

// Putting samples where they belong, while the program is still running.
//
// A sample is written to the SAMPLER's stream, carrying an anchor into the sampled thread's — which thread, and how
// far its stream had committed.
// That is what lets a consumer recover the trace, the ambient context and the open scope stack, none of which an id
// could give it, and it is why splicing exists at all rather than the sampler writing into the target directly.
//
// Offline that is `recording::spliced_samples`, over a recording that is already complete.
// A live consumer has no such luxury: it sees blocks as they are drained, and a sample's target may not have arrived.
//
// **So this trades a bounded delay for placement, and gives up rather than waiting forever.**
// A sample whose target never shows is forwarded where it was recorded, which is exactly what an offline splice does
// with one it cannot place.
// Losing the position of an occasional sample costs a statistical profile nothing; stalling the stream to chase it
// would cost the consumer its reason for being live.

namespace cc::rec
{
struct splice_options;
struct splicing_listener;
} // namespace cc::rec

struct cc::rec::splice_options
{
    /// Batches an unplaced sample is carried for before it is forwarded unspliced.
    ///
    /// One batch is usually enough — the sampler and its target are drained together — so this is the allowance for
    /// the case where they are not.
    /// **It bounds both the delay and the memory**, which is the only reason there is a limit rather than a wait.
    isize max_hold_batches = 4;
};

/// Splices samples into the blocks they anchor into, then forwards everything downstream.
///
/// Register THIS with the recorder, not the downstream listener.
/// Blocks are forwarded at the end of the batch they arrived in, so everything is delayed by one batch and samples by
/// at most `max_hold_batches`.
///
/// **The downstream listener must outlive this one**, which registering this and only this makes natural.
struct cc::rec::splicing_listener final : rec::listener
{
    explicit splicing_listener(rec::listener& downstream, rec::splice_options const& opts = {})
      : _downstream(downstream), _opts(opts)
    {
    }

    void on_chunk(rec::chunk_view const& view) override;
    void on_batch_end() override;

    [[nodiscard]] cc::string_view listener_name() const override { return "splicing"; }

    /// Forwards everything still held, spliced where it can be and unspliced where it cannot.
    ///
    /// Call this once nothing more is coming — after a final `flush_blocking` — or the last samples stay here.
    /// `on_batch_end` does the same thing for every batch before it, so this is only about the tail.
    void flush();

    /// Samples placed into the block they were taken from.
    [[nodiscard]] isize spliced_count() const { return _spliced; }

    /// Samples forwarded without ever finding their target.
    /// A steady stream of these means the delay is too short for this workload, not that anything is broken.
    [[nodiscard]] isize unplaced_count() const { return _unplaced; }

private:
    rec::listener& _downstream;
    rec::splice_options _opts;

    /// This batch's blocks, waiting for the batch to end so the samples in them can be placed.
    rec::recording _pending;

    /// Samples no block in an earlier batch could take, carried in case their target is still coming.
    rec::recording _held;

    isize _rounds_held = 0;
    isize _spliced = 0;
    isize _unplaced = 0;

    void release(rec::recording const& blocks);
};
