#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-shader-library/filesystem/watch.hh>
#include <shaped-shader-library/fwd.hh>

#include <memory>

/// What a filesystem that can notify is built from: one cancellable sink, and a prefix-keyed set of them.
/// Nothing here touches an OS — memory_filesystem fires it from write(), and mount_table uses a bare slot
/// to fan several child watches into one sink.

namespace slib::impl
{
/// One registered sink, cancellable with a hard guarantee: fire() and cancel() take the same lock, so once
/// cancel() has returned the sink is neither running nor callable again.
///
/// An atomic "alive" flag would not do — it can read true an instant before the subscriber dies, and the
/// call lands anyway. That race is the whole hazard, so the lock is the point.
class watch_slot final
{
public:
    explicit watch_slot(watch_sink sink) : _sink(cc::move(sink)) {}

    /// Calls the sink unless the slot was cancelled. Runs on whichever thread noticed the change.
    void fire() const;

    /// Unsubscribes; blocks until an in-flight fire() has returned.
    void cancel();

private:
    // Mutable so fire() stays const. Held across the sink deliberately: it is what makes cancel() a
    // guarantee rather than a hope, and a sink is required to be cheap.
    mutable cc::mutex<watch_sink> _sink;
};

/// The subscription every slib filesystem hands back: cancelling its slot is how the promise on
/// watch_subscription is kept.
struct slot_subscription final : watch_subscription::impl_base
{
    explicit slot_subscription(std::shared_ptr<watch_slot> slot) : slot(cc::move(slot)) {}
    ~slot_subscription() override { slot->cancel(); }

    std::shared_ptr<watch_slot> slot;
};

/// The sinks a filesystem has handed out, each scoped to a prefix. Self-pruning: an entry holds its slot
/// weakly, so dropping the subscription is what removes it.
class watch_registry final
{
public:
    /// Registers `sink` for changes under `prefix` (already normalized; empty = the root).
    [[nodiscard]] watch_subscription add(cc::string prefix, watch_sink sink);

    /// Fires every sink whose prefix contains `path` (already normalized). Call it *after* releasing any
    /// lock over the content that changed: a sink is arbitrary code, and it must be able to observe the
    /// change it is being told about.
    void fire_for(cc::string_view path) const;

private:
    struct entry
    {
        cc::string prefix;
        std::weak_ptr<watch_slot> slot; ///< weak: the subscription owns it, so dropping it prunes here
    };

    // Mutable so fire_for() stays const.
    mutable cc::mutex<cc::vector<entry>> _entries;
};
} // namespace slib::impl
