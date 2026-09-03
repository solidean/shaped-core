#pragma once

#include <clean-core/thread/async.hh>
#include <clean-net/common/error.hh>

/// The two pieces of `cc::async` plumbing every part of this library needs, in one place so they behave the same
/// everywhere.

namespace cnet
{
/// The failure-channel value a `cnet::error` becomes.
///
/// Every backend goes through this rather than wrapping the error itself, so one cancelled outcome is spelled the
/// same way throughout.
[[nodiscard]] cc::async_error to_async_error(error e);
} // namespace cnet

namespace cnet::impl
{
/// Run `fn(source)` once `source` has settled -- at once if it already has.
///
/// `fn` is what pushes into whatever promise the caller is holding, which is why this is the whole of the seam.
/// It fires on the thread that completed `source`, which for everything in this library is the reactor thread.
///
/// A one-shot completion hook rather than a `cc::async` compute frame, because frames need a scheduler and this
/// library deliberately has none: the reactor is the only place work runs.
template <class T, class F>
void when_ready(cc::shared_async<T> source, F fn)
{
    struct waiter
    {
        cc::shared_async<T> source;
        F fn;

        static void fire(void* ctx)
        {
            auto* const self = static_cast<waiter*>(ctx);
            self->fn(self->source);
            delete self;
        }
    };

    auto* const w = new waiter{cc::move(source), cc::move(fn)};
    if (w->source->install_completion_hook_or_ready(&waiter::fire, w))
        waiter::fire(w);
}

/// Hand one settled async's outcome to another promise, whatever it turned out to be.
template <class T>
void forward_outcome(cc::shared_async<T> const& from, cc::shared_async<T> const& to)
{
    if (from->has_error())
        to->push_error(from->propagate_error());
    else
        to->push_value(from->take_value());
}

/// A promise that is already broken, for a failure discovered before anything reached the reactor.
template <class T>
[[nodiscard]] cc::shared_async<T> failed_async(error e)
{
    auto promise = cc::make_async_manual<T>();
    promise->push_error(to_async_error(cc::move(e)));
    return promise;
}
} // namespace cnet::impl
