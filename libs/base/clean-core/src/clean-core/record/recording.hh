#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/platform/module_table.hh>
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

    /// Clamped at zero as a belt-and-braces measure: a live recording's stamps strictly increase, but a DESERIALIZED one
    /// carries whatever its file said, and a file is not something to trust arithmetic to.
    [[nodiscard]] u64 duration_cycles() const { return end_cycles > begin_cycles ? end_cycles - begin_cycles : 0; }

    /// The duration in seconds, or 0 without a calibrated cycle counter.
    [[nodiscard]] f64 duration_secs() const;
};

/// One recorded edge of the trace graph.
///
/// An edge is a fact, not a structure: the graph is whatever a reader builds out of the set, and nothing in the
/// recorder ever holds one.
/// That is what makes a LATE discovery free — an edge recorded once the relationship was learned is the same fact,
/// however far after the work it arrived.
struct cc::rec::trace_relation
{
    /// What the edge means.
    /// Never null on an edge that came out of a recording.
    rec::relation_type const* type = nullptr;

    /// The subject first, then its objects.
    /// One convention covers a fan-out (`parent_of(parent, children…)`) and a fan-in (`caused_by(effect, causes…)`)
    /// alike; for a symmetric type the order carries nothing and every member is a peer.
    cc::vector<rec::trace_id> members;

    u64 cycles = 0;

    [[nodiscard]] rec::trace_id subject() const { return members.empty() ? rec::trace_id::none : members.front(); }

    [[nodiscard]] cc::span<rec::trace_id const> objects() const
    {
        return members.empty() ? cc::span<rec::trace_id const>() : cc::span<rec::trace_id const>(members).subspan(1);
    }
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

/// What a bounded capture is allowed to keep.
///
/// A long-running capture has to throw something away, and which thing depends entirely on what the capture is FOR.
/// A crash ring wants the recent past whatever it costs; a background trace wants a memory ceiling it never breaches.
/// The two are not the same policy with different numbers, so both are expressible here rather than one being the
/// built-in and the other a workaround.
///
/// **Every limit is off at zero**, so a default-constructed policy keeps everything — which is what an ordinary
/// scoped capture wants.
///
/// Two shapes worth naming, because they are the ones people actually ask for:
///
///     {.max_secs = 30, .max_bytes = 128 << 20}       // the last 30s, and never more than 128 MB
///     {.guaranteed_secs = 20, .max_bytes = 64 << 20} // the last 20s WHATEVER it costs, then 64 MB for the rest
///
/// The difference is which one yields when they conflict, and that is the whole point of having both.
struct cc::rec::retention_policy
{
    /// Never evicted to satisfy `max_bytes`, however large it grows.
    ///
    /// This is the knob that makes a byte cap safe for forensics: without it a burst of logging can evict the very
    /// seconds before a crash, which is the only part anybody wanted.
    f64 guaranteed_secs = 0;

    /// Nothing older than this is kept, whatever the byte budget would allow.
    /// Outranks `guaranteed_secs` if both are set and they disagree, since a hard age limit is the stricter promise.
    f64 max_secs = 0;

    /// Evicted oldest-first to stay at or under this, `guaranteed_secs` permitting.
    /// Counts the bytes of the blocks held, which is what the chunk references are actually keeping alive.
    isize max_bytes = 0;

    [[nodiscard]] bool keeps_everything() const { return guaranteed_secs <= 0 && max_secs <= 0 && max_bytes <= 0; }
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

    /// Drops the oldest blocks until `policy` is satisfied, and returns how many went.
    ///
    /// **Block-granular**, so the result is approximate by design: a block is the unit a chunk reference keeps alive,
    /// and freeing anything smaller would free no memory at all.
    isize trim(rec::retention_policy const& policy);

    /// This recording with `policy` applied, leaving this one alone.
    [[nodiscard]] rec::recording retained(rec::retention_policy const& policy) const;

    /// Bytes of event stream this recording holds alive.
    [[nodiscard]] isize total_bytes() const;

    /// Appends a block that was built rather than captured.
    /// For a deserializer and for tests; ordinary code captures through a listener.
    void append_block(rec::recorded_block block);

    void clear() { _blocks.clear(); }

    /// The modules this recording's addresses are relative to.
    ///
    /// **Empty means "this process's own"**, which is the case for anything captured live, and is why nothing has to
    /// snapshot a table it is not going to need.
    /// A recording read back from a file carries the table the file recorded, because by then the process that gave
    /// those addresses meaning is gone.
    [[nodiscard]] cc::span<cc::loaded_module const> modules() const { return _modules; }
    void set_modules(cc::vector<cc::loaded_module> modules) { _modules = cc::move(modules); }

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
    ///
    /// **Identity, not name.** A deserialized recording owns its own domain objects, so this matches nothing on one;
    /// the by-name overload is what works on both.
    [[nodiscard]] rec::recording from_domain(rec::domain const* d) const;

    /// Only events from sites in a domain called `name`.
    /// The form that survives a round trip through a file.
    [[nodiscard]] rec::recording from_domain(cc::string_view name) const;

    /// Only events of one kind.
    [[nodiscard]] rec::recording of_kind(rec::event_kind k) const;

    /// Only events whose timestamp falls in `[begin_cycles, end_cycles)`.
    [[nodiscard]] rec::recording in_cycle_range(u64 begin_cycles, u64 end_cycles) const;

    /// Drops what finished before the cutoff, keeping scopes that are still open across it.
    /// What went is reported as a `dropped_span` event, so the result says what it no longer knows.
    [[nodiscard]] rec::recording decimated(rec::decimation_options const& options) const;

    /// Only what was recorded while a thread was under `id`.
    ///
    /// Trace membership is stream STATE: a thread publishes a delta on entering and leaving, and this carries the
    /// running value forward per thread.
    /// So a trace entered BEFORE the recording starts is not attributed — the enter has to be in here somewhere.
    [[nodiscard]] rec::recording from_trace(rec::trace_id id) const;

    /// Puts a sampling sideband back where it belongs.
    ///
    /// A sample is written to the SAMPLER's stream carrying an anchor — which thread it caught, and how far that
    /// thread's stream had committed — because writing into a suspended thread's stream would corrupt it.
    /// This moves each sample into the anchored thread's block at the anchored offset, so a reader replaying that
    /// stream has the trace, the ambient context and the open scopes already in hand when the sample arrives.
    ///
    /// A sample whose anchor names bytes this recording does not contain — dropped under an overflow policy, or simply
    /// not captured — stays where it was rather than being discarded.
    ///
    /// **Idempotent in position, not merely in count.**
    /// Splicing lifts samples only out of chunk-backed blocks; a synthesized block is where a previous splice — offline
    /// or `splicing_listener`'s — already put them, so it passes through untouched.
    /// That is what makes this safe to run in a pipeline that may have run it already.
    [[nodiscard]] rec::recording spliced_samples() const;

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

    /// Every recorded relation, in timestamp order.
    /// The trace graph is what a reader builds from these; the recorder never holds one.
    [[nodiscard]] cc::vector<rec::trace_relation> trace_relations() const;

    // what the recorder itself cost
public:
    /// The cycles this recording's own events cost to write, estimated from cc::rec::overhead().
    ///
    /// An event that measured itself — one carrying an end timestamp, as a stacktrace capture does — contributes its
    /// real cost rather than the model's.
    [[nodiscard]] f64 estimated_overhead_cycles() const;

    /// That, as a fraction of the recorded wall time summed over threads.
    ///
    /// Zero when the recording spans no time.
    /// Past a few percent, the recording is changing what it measures — the number the whole "annotate everywhere"
    /// argument rests on, so it is worth looking at rather than assuming.
    [[nodiscard]] f64 estimated_overhead_ratio() const;

private:
    cc::vector<rec::recorded_block> _blocks;
    cc::vector<cc::loaded_module> _modules;
};

/// A listener that captures everything it is offered into a recording.
///
/// The "record for a scope, then own the result" idiom: register one, do the work, unregister it, and the recording is
/// yours with no copying and no lifetime question.
struct cc::rec::recording_listener final : rec::listener
{
    recording_listener() = default;

    /// Captures under a bound, which is what makes a capture that runs for hours possible at all.
    /// Trimming happens as chunks arrive, so the listener never holds more than the policy allows.
    explicit recording_listener(rec::retention_policy const& policy) : _policy(policy) {}

    void on_chunk(rec::chunk_view const& view) override;

    [[nodiscard]] cc::string_view listener_name() const override { return "recording"; }

    [[nodiscard]] rec::recording const& result() const { return _recording; }

    /// Blocks retention has dropped so far.
    /// Nonzero means this recording has holes, which a reader has to know before concluding anything from a gap.
    [[nodiscard]] isize dropped_blocks() const { return _dropped_blocks; }

    /// Moves the recording out, leaving this listener empty and ready to capture again.
    [[nodiscard]] rec::recording take();

private:
    rec::recording _recording;
    rec::retention_policy _policy;
    isize _dropped_blocks = 0;
};
