#pragma once

#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/http/message.hh>
#include <clean-net/io/io_system.hh>

// What every HTTP backend shares about delivering a body: the gate behind `cnet::resume_body`, and the body cap.

/// What a `cnet::resume_body` holds.
///
/// Shared with the request rather than owned by it, so a sink that kept its `resume_body` past the end of the
/// request finds a gate that has been disarmed rather than a freed request.
struct cnet::impl::body_gate
{
    cc::atomic<i32> references = 1;

    struct data
    {
        /// Null once the request has settled, which is what makes a late `resume()` harmless.
        io_system* io = nullptr;

        /// The manual operation the paused request waits on, or null when it is not paused.
        impl::io_operation* waiting = nullptr;

        /// A resume that arrived before the pause had published its operation.
        ///
        /// The sink returns on the reactor thread and may hand its `resume_body` to a consumer on another one, so
        /// the two genuinely race -- and a signal posted ahead of the operation it means to wake is dropped.
        /// The pause re-reads this after publishing, which is the same shape `cancel_registration::attach` uses.
        bool resume_pending = false;
    };

    cc::mutex<data> state;
};

namespace cnet::impl
{
/// What `request_options::max_body_bytes` means, once: a cap, the platform default, or no cap at all.
[[nodiscard]] i64 resolve_body_cap(i64 requested);
} // namespace cnet::impl
