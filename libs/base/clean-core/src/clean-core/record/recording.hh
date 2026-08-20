#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
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

/// One matched pair of scope events, which is what a flame graph is drawn from.
struct cc::rec::scope_span
{
    /// The descriptor of the opening event, so the site is identifiable beyond its name.
    rec::desc const* desc = nullptr;

    u64 begin_cycles = 0;
    u64 end_cycles = 0;
    u32 depth = 0;

    cc::thread_id thread = cc::thread_id::invalid;

    /// True when no closing event was found, which is what an unfinished or truncated stream leaves behind.
    bool is_open = false;

    [[nodiscard]] cc::string_view name() const { return desc->name; }

    /// Clamped at zero, because two cycle readings around a very short span can come back inverted.
    [[nodiscard]] u64 duration_cycles() const { return end_cycles > begin_cycles ? end_cycles - begin_cycles : 0; }

    /// The duration in seconds, or 0 without a calibrated cycle counter.
    [[nodiscard]] f64 duration_secs() const;
};

/// What `recording::decimated` throws away, and what it leaves in its place.
struct cc::rec::decimation_options
{
    /// Drop everything that finished before this cycle reading.
    u64 keep_from_cycles = 0;

    /// Keep scopes that are still open at the cutoff, however old they are.
    /// Without this a decimated trace loses the frame it is sitting inside, which is the one you wanted.
    bool keep_open_scopes = true;

    /// Replace what was dropped with one dropped_span event per thread.
    /// The point of the whole exercise: after decimating, a reader can still tell "nothing happened" from
    /// "we stopped looking".
    bool report_dropped = true;
};

/// One captured block: the bytes, plus everything needed to rebuild the view over them.
///
/// A block either BORROWS a live chunk — the capture case, which copies nothing — or owns a buffer of its own, which
/// is what filtering produces since the events it keeps are no longer contiguous.
/// Exactly one of `source` and `owned` is set.
struct cc::rec::recorded_block
{
    /// Keeps the bytes alive.
    /// This reference IS the capture.
    rec::chunk_ref source;

    /// The synthesized bytes of a filtered or decimated block.
    cc::pinned_data<byte const> owned;

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

    /// The bytes this block covers, wherever they live.
    [[nodiscard]] cc::span<byte const> bytes() const;

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

    // the algebra
public:
    /// A new recording holding only the events `pred` accepts.
    ///
    /// The kept events are no longer contiguous, so this SYNTHESIZES blocks rather than borrowing — the result owns
    /// its bytes and no longer pins the chunks it came from.
    [[nodiscard]] rec::recording filtered(cc::function_ref<bool(rec::chunk_view const&, rec::event_view const&)> pred) const;

    /// Only what one thread recorded.
    /// **This is how a synchronous capture is narrowed to the code under test**, until ambient filtering makes the
    /// asynchronous case answerable too.
    [[nodiscard]] rec::recording from_thread(cc::thread_id id) const;

    /// Only events from sites in `d`.
    [[nodiscard]] rec::recording from_domain(rec::domain const* d) const;

    /// Only events of one kind.
    [[nodiscard]] rec::recording of_kind(rec::event_kind k) const;

    /// Only events whose timestamp falls in `[begin_cycles, end_cycles)`.
    [[nodiscard]] rec::recording in_cycle_range(u64 begin_cycles, u64 end_cycles) const;

    /// Drops what finished before the cutoff, keeping scopes that are still open across it.
    /// What went is reported as a `dropped_span` event, so the result says what it no longer knows.
    [[nodiscard]] rec::recording decimated(rec::decimation_options const& options) const;

    // queries
public:
    /// How many events carry this name.
    [[nodiscard]] isize count(cc::string_view name) const;

    [[nodiscard]] bool contains(cc::string_view name) const { return count(name) > 0; }

    [[nodiscard]] isize count_of_kind(rec::event_kind k) const;

    /// The `value` field of the first / last event with this name, as a double.
    /// Empty when nothing recorded it, or what did carries no numeric `value`.
    [[nodiscard]] cc::optional<f64> first_value(cc::string_view name) const;
    [[nodiscard]] cc::optional<f64> last_value(cc::string_view name) const;

    /// Every numeric `value` recorded under this name, in timestamp order.
    [[nodiscard]] cc::vector<f64> values(cc::string_view name) const;

    /// The `value` field of the first event with this name, as text.
    [[nodiscard]] cc::optional<cc::string> first_text(cc::string_view name) const;

    /// Whether these names appear in this order, with anything at all allowed between them.
    /// Ordered by TIMESTAMP rather than by block, so it means the same thing across threads as within one.
    [[nodiscard]] bool contains_in_order(cc::span<cc::string_view const> names) const;

    /// Every log message, rendered as it would print — the formatted payload, or the descriptor's text when the site
    /// took no arguments.
    [[nodiscard]] cc::vector<cc::string> messages() const;

    /// Every matched scope pair, in timestamp order.
    /// A scope whose close is missing comes back with `is_open` set rather than being dropped.
    [[nodiscard]] cc::vector<rec::scope_span> scopes() const;

    /// The matched scope pairs with this name.
    [[nodiscard]] cc::vector<rec::scope_span> scopes(cc::string_view name) const;

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
