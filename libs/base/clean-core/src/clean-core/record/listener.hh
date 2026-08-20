#pragma once

#include <clean-core/record/event_view.hh>
#include <clean-core/record/fwd.hh>
#include <clean-core/string/string_view.hh>

// What consumes recorded events, and the rule that keeps a listener recording from feeding itself.
//
// The raw interface is per CHUNK rather than per event, because that is what keeps the consumer cheap: one virtual
// call per megabyte, not one per event.
// The adapters below buy back per-event ergonomics for listeners that would rather have them.
//
// **Every listener callback runs under one global processing mutex**, so a listener never needs a lock of its own and
// never sees two callbacks at once — whether the actor or an explicit flush is driving.

namespace cc::rec
{
[[nodiscard]] rec::listener_handle register_listener(rec::listener& l);
void unregister_listener(rec::listener_handle h);
} // namespace cc::rec

/// Registration handle.
/// Unregistering through it is what makes it safe to destroy the listener.
///
/// The index inside is the listener's LAYER, so a handle also says where in the layer order it sits.
struct cc::rec::listener_handle
{
    [[nodiscard]] bool is_valid() const { return _index >= 0; }

    /// This listener's layer: events recorded from inside it reach only listeners below this index.
    [[nodiscard]] isize layer() const { return _index; }

private:
    friend rec::listener_handle rec::register_listener(rec::listener&);
    friend void rec::unregister_listener(rec::listener_handle);

    isize _index = -1;
    u64 _generation = 0;
};

/// A consumer of recorded events.
///
/// Derive and override on_chunk; everything else has a working default.
/// A listener MAY record events of its own — logging from a listener is entirely normal — and the layer rule below is
/// what stops that from becoming a cycle.
struct cc::rec::listener
{
    virtual ~listener() = default;

    /// One block of one thread's events, in the order that thread wrote them.
    ///
    /// Blocks from different threads arrive in no particular order relative to each other; wrap the listener in
    /// cc::rec::ordered_listener when that matters.
    virtual void on_chunk(rec::chunk_view const& view) = 0;

    /// Called after every batch the consumer drained, whether or not anything arrived.
    /// The place to flush a buffer of your own.
    virtual void on_batch_end() {}

    /// What this listener is called in diagnostics.
    [[nodiscard]] virtual cc::string_view listener_name() const { return "listener"; }
};

/// Feeds a listener one event at a time instead of one block at a time.
///
/// CRTP rather than virtual, so the per-event dispatch inlines: derive as
/// `struct my_listener : cc::rec::event_listener<my_listener>` and provide `void on_event(chunk_view const&, event_view const&)`.
template <class Derived>
struct cc::rec::event_listener : rec::listener
{
    void on_chunk(rec::chunk_view const& view) final
    {
        auto& self = static_cast<Derived&>(*this);
        for (auto it = view.begin(); it != view.end(); ++it)
            self.on_event(view, *it);
    }
};
