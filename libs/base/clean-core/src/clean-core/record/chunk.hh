#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/record/fwd.hh>
#include <clean-core/thread/atomic.hh>

// cc::rec::chunk — the unit of recording, of ownership and of capture.
//
// A thread writes into exactly one chunk at a time and publishes its progress through a single release-stored
// watermark, which is the only cross-thread word on the write path.
// Chunks are refcounted, and **holding references IS the capture mechanism**: a ring buffer of recent activity, a
// crash dump and a test's recording are all "keep these chunks alive", with no copying anywhere.

/// One thing a chunk keeps alive on behalf of an event that points into it.
///
/// Uniform across everything that can be pinned — a `cc::pinned_data`'s owner above all — so the chunk never learns
/// what it is holding.
/// The pinned object must be immutable for as long as the chunk lives; that is the pinning caller's promise.
struct cc::rec::pin
{
    void* object = nullptr;
    void (*release)(void* object) = nullptr;
};

/// A block of event bytes owned by one thread, plus the metadata that makes it independently decodable.
///
/// `committed` is the contract with the consumer: everything below it is a whole number of complete events, and the
/// release store that publishes it happens-after the bytes were written.
/// So a consumer reading a LIVE chunk can never see a torn event — the worst it sees is one event's worth of lag.
struct cc::rec::chunk
{
    /// How many pins fit in the chunk header before one slot is spent on a spill block.
    ///
    /// Not a limit: a full array is moved into a heap block, and the block itself becomes a single pin — so 63 slots
    /// come free and the trick repeats without bound.
    /// Sixty-four is what makes that rare rather than what caps it.
    static constexpr isize pin_capacity = 64;

    /// The layer value of a chunk written by ordinary code rather than from inside a listener.
    static constexpr u16 no_layer = 0xFFFF;

    // the write path's one shared word
public:
    /// Bytes of `data` that hold complete, published events.
    /// Release-stored by the owner after each event, acquire-loaded by the consumer.
    alignas(64) cc::atomic<u32> committed = 0;

    // producer-published
public:
    /// The owner's next chunk, published when it rotates.
    /// Null while this is the newest.
    alignas(64) cc::atomic<chunk*> next_in_thread = nullptr;

    /// Entries of `pins` that are filled.
    /// Release-stored, so the consumer sees the pin before it sees the event.
    cc::atomic<u32> pin_count = 0;

    /// True once the owner will write no more into this chunk.
    cc::atomic<bool> is_sealed = false;

    // immutable between claim and release
public:
    alignas(64) byte* data = nullptr;
    u32 capacity = 0;

    /// The listener layer this chunk was written under, or no_layer.
    /// See libs/base/clean-core/docs/systems/recording.md on re-entrancy.
    u16 layer = no_layer;

    /// This chunk's position in its thread's sequence, so a gap in a dump is detectable.
    u64 seq = 0;

    /// The (cycles, wall) pair taken when the chunk was claimed, and again when it was sealed.
    /// Interpolating between them is how a cycle count becomes a timestamp; a live chunk has only the first pair.
    u64 base_cycles = 0;
    f64 base_wall_secs = 0;
    u64 seal_cycles = 0;
    f64 seal_wall_secs = 0;

    rec::impl::thread_state* owner = nullptr;
    rec::chunk_pool* pool = nullptr;

    // lifetime
public:
    void retain() { refs.fetch_add(1, cc::memory_order_relaxed); }

    /// Drops one reference, returning the chunk to its pool at zero.
    void release_ref();

    [[nodiscard]] i32 ref_count() const { return refs.load(cc::memory_order_relaxed); }

    // access
public:
    /// The published prefix: whole events only, safe to read while the owner is still writing.
    [[nodiscard]] cc::span<byte const> committed_bytes() const
    {
        return cc::span<byte const>(data, isize(committed.load(cc::memory_order_acquire)));
    }

    /// Adds a pin, spilling the array into a heap block first when it is full.
    ///
    /// Owner-only, and returns false only when that block could not be allocated — at which point the caller records
    /// the value inline instead of by pin.
    /// **This can allocate**, which is why it is only ever reached from a site that asked to pin something.
    bool try_add_pin(rec::pin p);

    /// The pins filled so far, safe to read alongside a live chunk.
    [[nodiscard]] cc::span<rec::pin const> published_pins() const
    {
        return cc::span<rec::pin const>(pins, isize(pin_count.load(cc::memory_order_acquire)));
    }

    /// Releases every pin and forgets them.
    /// Called when the chunk is recycled, never while anything can still read it.
    void release_pins();

public:
    cc::atomic<i32> refs = 1;

private:
    rec::pin pins[pin_capacity] = {};
};

/// An owning reference to a chunk, so a recording is a value type.
struct cc::rec::chunk_ref
{
    chunk_ref() = default;

    /// Takes a reference; use `adopt` for one you already own.
    explicit chunk_ref(rec::chunk* c) : _c(c)
    {
        if (_c != nullptr)
            _c->retain();
    }

    chunk_ref(chunk_ref const& rhs) : chunk_ref(rhs._c) {}
    chunk_ref(chunk_ref&& rhs) noexcept : _c(rhs._c) { rhs._c = nullptr; }
    chunk_ref& operator=(chunk_ref const& rhs)
    {
        if (rhs._c != nullptr)
            rhs._c->retain(); // before the release, so self-assignment is safe
        _reset();
        _c = rhs._c;
        return *this;
    }
    chunk_ref& operator=(chunk_ref&& rhs) noexcept
    {
        if (this != &rhs)
        {
            _reset();
            _c = rhs._c;
            rhs._c = nullptr;
        }
        return *this;
    }
    ~chunk_ref() { _reset(); }

    /// Wraps a reference the caller already owns, without taking another.
    [[nodiscard]] static chunk_ref adopt(rec::chunk* c)
    {
        chunk_ref r;
        r._c = c;
        return r;
    }

    [[nodiscard]] rec::chunk* get() const { return _c; }
    [[nodiscard]] rec::chunk* operator->() const { return _c; }
    [[nodiscard]] explicit operator bool() const { return _c != nullptr; }

private:
    void _reset()
    {
        if (_c != nullptr)
            _c->release_ref();
        _c = nullptr;
    }

    rec::chunk* _c = nullptr;
};
