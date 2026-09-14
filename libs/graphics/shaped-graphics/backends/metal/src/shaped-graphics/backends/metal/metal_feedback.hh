#pragma once

#include <clean-core/string/string_view.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/context/device_error.hh>

/// The detachable end of a commit-feedback handler.
///
/// **This exists for a lifetime problem rather than for tidiness.**
/// A commit's feedback handler runs on a dispatch queue of Metal's choosing, at a time nothing in sg controls — after
/// the GPU work finishes, which can be after `shutdown` has returned and the context has been destroyed.
/// A handler capturing the context directly would then report into freed memory.
///
/// So a handler captures one of these instead, by shared_ptr, and `detach` at shutdown makes every handler still in
/// flight a no-op.
/// The mutex is what makes "detach" and "a handler is running right now" exclusive rather than a race of their own.
class sg::backend::metal::metal_feedback_sink
{
public:
    explicit metal_feedback_sink(metal_context& ctx) : _context(&ctx) {}

    /// Route one commit's error to the context, if it is still there.
    void report(sg::device_error_kind kind, cc::string_view message);

    /// Stop routing.
    /// Called from shutdown, before the context goes.
    /// A handler that runs afterwards finds nothing and does nothing.
    void detach();

private:
    cc::mutex<metal_context*> _context;
};

namespace sg::backend::metal
{
/// How an MTLCommandBufferError maps onto sg's coarse deferred-error vocabulary.
///
/// Most of Metal's codes describe one command buffer failing, which is `validation`'s bucket — "something already
/// recorded or submitted went wrong".
/// The three that mean the device itself is gone map to `device_lost`, which is sticky and which a caller recovers
/// from only by tearing the context down.
[[nodiscard]] sg::device_error_kind device_error_kind_of(NS::UInteger command_buffer_error_code);
} // namespace sg::backend::metal
