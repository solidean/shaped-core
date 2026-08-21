#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/record/chunk.hh>
#include <clean-core/record/desc.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/impl/writer_tls.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/atomic.hh>

#include <type_traits>

// The write path.
// Everything else in record/ exists to make this cheap.
//
// An enabled site is a gate, a timestamp, a bounds check, a copy and one release store.
// A disabled site is the gate alone — one load through the domain and a predictable branch — which is what makes it
// affordable to leave annotation in the code forever.
//
// **Recording does nothing until cc::rec::initialize() has run.**
// Events written before that are counted as drops and reported in the first gap event, rather than silently absent.

/// The fixed prefix of every event, laid out so the whole thing is one aligned store pair.
///
/// The payload follows immediately and is padded to eight bytes, so the next header is aligned too — `payload_size`
/// is the real size, and a decoder advances by the padded one.
struct cc::rec::impl::event_header
{
    rec::desc const* desc;
    u64 cycles;
    u32 payload_size;
    u16 core;
    u16 flags;
};

namespace cc::rec::impl
{
static_assert(sizeof(event_header) == 24, "the event header must stay 24 B, and 8-aligned for the payload after it");

// Event header flags.
inline constexpr u16 flag_none = 0;
inline constexpr u16 flag_has_end_cycles = 1 << 0; ///< the payload opens with a u64 giving when recording finished
inline constexpr u16 flag_has_stacktrace = 1 << 1; ///< the payload carries captured return addresses
inline constexpr u16 flag_payload_pinned = 1 << 2; ///< the payload references bytes held alive by a chunk pin
inline constexpr u16 flag_truncated = 1 << 3;      ///< the payload did not fit and was cut short
inline constexpr u16 flag_interned_stack = 1 << 4; ///< the stacktrace array holds ONE id, not addresses

/// Whether events carry the core they were recorded on, which costs about ten cycles per event.
extern cc::atomic<bool> g_capture_core_id;

/// Rounds a payload size up to the event stream's eight-byte grid.
[[nodiscard]] constexpr isize padded_payload(isize size)
{
    return (size + 7) & ~isize(7);
}

/// What one event costs the stream, header included.
[[nodiscard]] constexpr isize event_bytes_for(isize payload_size)
{
    return isize(sizeof(event_header)) + padded_payload(payload_size);
}

/// The timestamp an event is stamped with, and the core it was taken on when the policy asks for one.
[[nodiscard]] CC_FORCE_INLINE u64 record_timestamp(u16& core_out)
{
    if (g_capture_core_id.load(cc::memory_order_relaxed))
    {
        u32 core = 0;
        auto const cycles = cc::current_cycles_and_core(core);
        core_out = u16(core);
        return cycles;
    }
    core_out = 0;
    return cc::current_cycles();
}

/// The cold path: seal, rotate, register, or give up.
/// Returns true when the calling thread now has at least `needed` bytes free.
CC_COLD_FUNC bool writer_rotate(isize needed);

/// Accounts one event this thread could not write, for the next gap event to report.
CC_COLD_FUNC void writer_account_drop(isize bytes, u64 cycles);

/// Writes header and payload at the cursor and publishes them; the space must already be reserved.
CC_FORCE_INLINE void write_event_at(rec::desc const& d, u64 cycles, u16 core, u16 flags, void const* payload, isize payload_size)
{
    auto& w = t_writer;

    auto* const header = reinterpret_cast<event_header*>(w.cur);
    header->desc = &d;
    header->cycles = cycles;
    header->payload_size = u32(payload_size);
    header->core = core;
    header->flags = flags;

    if (payload_size > 0)
        cc::memcpy(w.cur + sizeof(event_header), payload, size_t(payload_size));

    w.cur += event_bytes_for(payload_size);
    w.current->committed.store(u32(w.cur - w.current->data), cc::memory_order_release);
}

/// The complete write, gate included.
/// This is what a recording macro compiles down to.
CC_FORCE_INLINE void record_bytes(rec::desc const& d, void const* payload, isize payload_size)
{
    if ((d.dom->enabled_mask() & d.enable_bit) == 0)
        return;

    auto const needed = event_bytes_for(payload_size);

    auto& w = t_writer;
    if (w.cur + needed > w.end) [[unlikely]]
    {
        if (!writer_rotate(needed))
        {
            writer_account_drop(needed, cc::current_cycles());
            return;
        }
    }

    // Stamped AFTER the bounds check, not before.
    // The cold path writes bookkeeping events of its own, and a timestamp taken ahead of it would sort this event
    // before them — so a thread's stream would go backwards at every rotation.
    u16 core = 0;
    auto const cycles = record_timestamp(core);

    write_event_at(d, cycles, core, flag_none, payload, payload_size);
}
} // namespace cc::rec::impl

namespace cc::rec
{
/// Whether this site would record anything right now.
///
/// **This is the entire cost of a disabled site**: one load of the domain mask and one AND.
/// The load goes through the descriptor to the domain rather than to a per-site word, so reconfiguring a domain is one
/// word write that reaches every site under it at once, with no registry to walk.
[[nodiscard]] CC_FORCE_INLINE bool is_recording(rec::desc const& d)
{
    return (d.dom->enabled_mask() & d.enable_bit) != 0;
}

/// Writes one complete event, copying `payload` inline.
CC_FORCE_INLINE void record_event(rec::desc const& d, cc::span<byte const> payload)
{
    impl::record_bytes(d, payload.data(), payload.size());
}

/// Writes one event with no payload — the whole content is its descriptor.
CC_FORCE_INLINE void record_event(rec::desc const& d)
{
    impl::record_bytes(d, nullptr, 0);
}

/// Writes one event whose payload is a trivially-copyable struct laid out by the descriptor fields.
template <class PayloadT>
CC_FORCE_INLINE void record_event(rec::desc const& d, PayloadT const& payload)
    requires(std::is_trivially_copyable_v<PayloadT> && !std::is_convertible_v<PayloadT const&, cc::span<byte const>>)
{
    impl::record_bytes(d, &payload, isize(sizeof(PayloadT)));
}

/// Reserves an event of up to `max_payload` bytes, to be filled and committed by the caller.
/// Returns a closed writer when the site is disabled or the stream could not take the event.
[[nodiscard]] rec::event_writer open_event(rec::desc const& d, isize max_payload);
} // namespace cc::rec

/// A reserved but unpublished event whose payload the caller fills in place.
///
/// This is what a formatted log message wants: reserve the remaining space, format straight into it, then publish only
/// the bytes that were actually written — no temporary buffer and no copy.
/// An open writer that is never committed leaves the chunk untouched, which is what makes abandoning one safe.
///
/// **Nothing else may record on this thread while a writer is open**, since both would claim the same cursor.
struct cc::rec::event_writer
{
    event_writer() = default;

    event_writer(event_writer const&) = delete;
    event_writer& operator=(event_writer const&) = delete;

    /// Movable, so open_event can hand one back; the reservation travels and the source is left closed.
    event_writer(event_writer&& rhs) noexcept
      : _base(rhs._base), _capacity(rhs._capacity), _desc(rhs._desc), _cycles(rhs._cycles), _core(rhs._core)
    {
        rhs._base = nullptr;
    }
    event_writer& operator=(event_writer&& rhs) noexcept
    {
        _base = rhs._base;
        _capacity = rhs._capacity;
        _desc = rhs._desc;
        _cycles = rhs._cycles;
        _core = rhs._core;
        rhs._base = nullptr;
        return *this;
    }

    /// True when space was reserved; false when recording is off or the stream could not take the event.
    [[nodiscard]] bool is_open() const { return _base != nullptr; }

    /// Where to write the payload, sized to what the chunk can actually take.
    /// May be shorter than the requested maximum, so a caller formatting into it must handle truncation.
    [[nodiscard]] cc::span<byte> payload() const
    {
        return cc::span<byte>(_base + sizeof(rec::impl::event_header), _capacity);
    }

    /// Publishes the event with `payload_size` bytes written.
    /// A size past the reservation is clamped, and the event is flagged truncated.
    void commit(isize payload_size, u16 extra_flags = rec::impl::flag_none);

private:
    friend rec::event_writer rec::open_event(rec::desc const&, isize);

    byte* _base = nullptr;
    isize _capacity = 0;
    rec::desc const* _desc = nullptr;
    u64 _cycles = 0;
    u16 _core = 0;
};

/// Redirects this thread's recording into listener-layer chunks for the duration of one listener callback.
///
/// A chunk carries exactly one layer, so entering a layer swaps the write cursor rather than tagging events, and the
/// listener's own output lands in chunks the dispatcher will only offer to LOWER layers.
///
/// **Nested dispatch records nothing.**
/// A listener may record; a listener reached by another listener's recording may not, and its events are counted as
/// drops.
/// That is what makes the layer rule terminate with no cycle detector.
struct cc::rec::impl::listener_layer_scope
{
    explicit listener_layer_scope(u16 layer);
    ~listener_layer_scope();

    listener_layer_scope(listener_layer_scope const&) = delete;
    listener_layer_scope& operator=(listener_layer_scope const&) = delete;

private:
    byte* _saved_cur = nullptr;
    byte* _saved_end = nullptr;
    rec::chunk* _saved_current = nullptr;
    rec::impl::thread_state* _saved_state = nullptr;
    u16 _saved_layer_plus_one = 0;
};

namespace cc::rec
{
/// Seals whatever this thread is writing into and hands it to the consumer.
/// Called for you when a thread exits; call it by hand only to bound how long this thread's tail sits unconsumed.
void seal_current_thread_chunk();

/// Names the calling thread in every recording it appears in.
/// Independent of cc::set_current_thread_name, which the OS owns; this one is what a trace viewer shows.
void set_current_thread_record_name(cc::string_view name);
} // namespace cc::rec
