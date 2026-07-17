#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/function/unique_function.hh>

#include <memory>

namespace slib
{
/// Called when something under a watched prefix MAY have changed. Fires from an arbitrary thread — often
/// an OS watcher's — so it must be cheap and non-blocking: enqueue and return.
///
/// A hint to rescan, never a description of what changed. See filesystem::watch.
using watch_sink = cc::unique_function<void()>;

/// A live watch; destroying it unsubscribes.
///
/// Once the destructor has returned, the sink is neither running nor callable again. That guarantee is
/// what lets a subscriber keep a subscription beside the state its sink touches and just drop it on the
/// way out — a sink landing one instruction late is the classic use-after-free in every file watcher.
///
/// Move-only. A default-constructed one is valid and never fires, which is the honest answer for content
/// that cannot change (embedded_filesystem). That is *not* the same as filesystem::watch returning
/// nullopt, which means "I cannot notify at all — poll me".
class watch_subscription final
{
public:
    /// Whatever a filesystem must keep alive for the watch. Destroying it completes the unsubscribe, so
    /// an implementation's destructor is where the guarantee above is paid for.
    struct impl_base
    {
        virtual ~impl_base() = default;
    };

    /// The inert subscription: valid, never fires.
    watch_subscription() = default;

    explicit watch_subscription(std::unique_ptr<impl_base> impl) : _impl(cc::move(impl)) {}

    watch_subscription(watch_subscription&&) = default;
    watch_subscription& operator=(watch_subscription&&) = default;
    watch_subscription(watch_subscription const&) = delete;
    watch_subscription& operator=(watch_subscription const&) = delete;

private:
    /// std::unique_ptr, not cc::unique_ptr: ownership is polymorphic through impl_base. Null when inert.
    std::unique_ptr<impl_base> _impl;
};
} // namespace slib
