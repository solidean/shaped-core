#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/record/chunk.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/string/string.hh>

// A recording is a VALUE.
// That is the idea the whole system is built around.
//
// Capturing costs no copying: a recording holds references to the chunks it covers, and holding a chunk reference is
// what keeps it out of the pool.
// So "the last five frames", a crash dump and a test's expectations are the same mechanism, and a recording can be
// passed around, concatenated, replayed and queried like any other value.
//
// **A recording is process-local.** Events point at descriptors, and descriptors are static objects in this binary.
// Serializing one means resolving those pointers, which is what record/serialize.hh is for.

/// One captured block: the bytes, plus everything needed to rebuild the view over them.
struct cc::rec::recorded_block
{
    /// Keeps the bytes alive.
    /// This reference IS the capture.
    rec::chunk_ref source;

    u32 from = 0;
    u32 to = 0;

    cc::thread_id thread_id = cc::thread_id::invalid;
    u32 thread_index = 0;

    /// Copied rather than borrowed: a thread can be renamed, and a recording must not change afterwards.
    cc::string thread_name;

    u64 chunk_seq = 0;
    u16 layer = 0xFFFF;

    u64 base_cycles = 0;
    f64 base_wall_secs = 0;
    u64 seal_cycles = 0;
    f64 seal_wall_secs = 0;

    /// The stream state at the start of the SOURCE CHUNK, which is this block's own only when `from` is zero.
    rec::stream_state const* state_at_start = nullptr;

    /// Rebuilds the view this block was captured from.
    [[nodiscard]] rec::chunk_view view() const;
};

/// A captured set of event blocks, held alive and passed around by value.
struct cc::rec::recording
{
    recording() = default;

    // building
public:
    /// Captures `view`, taking a reference to the chunk behind it.
    void append(rec::chunk_view const& view);

    /// Appends every block of `other`, in order.
    void append(rec::recording const& other);

    void clear() { _blocks.clear(); }

    // reading
public:
    [[nodiscard]] cc::span<rec::recorded_block const> blocks() const { return _blocks; }
    [[nodiscard]] isize block_count() const { return _blocks.size(); }
    [[nodiscard]] bool empty() const;

    /// How many events this recording holds.
    /// Walks every block, so it is a query rather than a field.
    [[nodiscard]] isize event_count() const;

    /// Calls `f` for every event, block by block in capture order.
    void for_each_event(cc::function_ref<void(rec::chunk_view const&, rec::event_view const&)> f) const;

    /// Feeds every block to `l`, exactly as the live system would have.
    /// The listener need not be registered — replay is what makes a recording testable offline.
    void replay(rec::listener& l) const;

private:
    cc::vector<rec::recorded_block> _blocks;
};

/// A listener that captures everything it is offered into a recording.
///
/// The "record for a scope, then own the result" idiom: register one, do the work, unregister it, and the recording is
/// yours with no copying and no lifetime question.
struct cc::rec::recording_listener final : rec::listener
{
    void on_chunk(rec::chunk_view const& view) override { _recording.append(view); }

    [[nodiscard]] cc::string_view listener_name() const override { return "recording"; }

    [[nodiscard]] rec::recording const& result() const { return _recording; }

    /// Moves the recording out, leaving this listener empty and ready to capture again.
    [[nodiscard]] rec::recording take();

private:
    rec::recording _recording;
};
