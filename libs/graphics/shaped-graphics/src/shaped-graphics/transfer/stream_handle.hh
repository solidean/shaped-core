#pragma once

#include <clean-core/common/utility.hh> // cc::unit
#include <clean-core/error/optional.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>

/// How far a streaming transfer has got.
/// `total_hint` is exactly that — a hint, absent when the source cannot know its own size up front, and never
/// something to compute a completion test from.
/// Ask the handle, or its completion async, whether the transfer is done.
struct sg::stream_progress
{
    i64 bytes_done = 0;
    cc::optional<i64> total_hint;
};

namespace sg::impl
{
/// The state one streaming transfer shares between its handle and the copy actor.
///
/// Everything the handle writes is a relaxed atomic, so `set_priority` and `cancel` are a store from any thread with
/// no message and no lock; the actor reads them when it next picks a job, which is once per window.
///
/// `completion` is created with the job and never reassigned, so handing it out from any thread is safe.
struct stream_control
{
    std::atomic<i32> priority = 0;
    std::atomic<bool> cancelled = false;
    std::atomic<bool> promoted = false; // promote_to_async: the transfer additionally gains the automatic waits
    std::atomic<i64> bytes_done = 0;
    std::atomic<i64> total_hint = -1; // < 0 == unknown

    /// Settled by the actor: a value once the last chunk has landed, `async_error::make_cancelled()` otherwise.
    /// Settling it is mandatory on EVERY teardown path — a manual node nobody pushes parks its dependents forever.
    cc::shared_async<cc::unit> completion;

    /// The backend's half of promote_to_async: stamps the destination so later-recorded command lists wait on this
    /// transfer.
    /// Installed with the job and invoked at most once, from whatever thread promotes.
    /// Backend-specific, hence a callable rather than something this struct could do itself.
    cc::unique_function<void()> on_promote;

    [[nodiscard]] stream_progress progress() const
    {
        stream_progress p;
        p.bytes_done = bytes_done.load(std::memory_order_relaxed);
        if (auto const t = total_hint.load(std::memory_order_relaxed); t >= 0)
            p.total_hint = t;
        return p;
    }
};

/// The half of a streaming handle both directions share, so the two differ only in what they deliver.
/// Move-only, and **dropping it cancels the transfer** — a stream always has an observer, which is what pays for its
/// weaker guarantee.
/// For fire-and-forget with automatic synchronization, use ctx.upload instead.
class stream_handle_base
{
public:
    stream_handle_base() = default;
    explicit stream_handle_base(std::shared_ptr<stream_control> control) : _control(cc::move(control)) {}

    ~stream_handle_base() { cancel(); }

    stream_handle_base(stream_handle_base&& rhs) noexcept : _control(cc::move(rhs._control)) {}

    /// Assigning over a live handle cancels what it was holding.
    /// A defaulted move would release that control silently, which is the one way to lose a transfer's last observer
    /// without cancelling it — and reassigning one handle across a loop is the ordinary way to hit it.
    stream_handle_base& operator=(stream_handle_base&& rhs) noexcept
    {
        if (this != &rhs)
        {
            cancel();
            _control = cc::move(rhs._control);
        }
        return *this;
    }

    stream_handle_base(stream_handle_base const&) = delete;
    stream_handle_base& operator=(stream_handle_base const&) = delete;

    /// Whether this handle is backed by a real transfer (vs default-constructed or moved from).
    [[nodiscard]] bool is_valid() const { return _control != nullptr; }

    /// Stops the transfer being served from here on.
    /// Chunks already recorded still run — their staging bytes are committed — so this bounds future work rather than
    /// undoing past work, and the claimed extent stays yours until the handle reports settled.
    /// Idempotent, and safe from any thread.
    void cancel()
    {
        if (_control != nullptr)
            _control->cancelled.store(true, std::memory_order_relaxed);
    }

    /// Reorders this transfer against the other streaming ones; higher runs first.
    /// Takes effect at the next window, so within roughly one window's time.
    /// Ignored once the transfer has settled, and safe from any thread.
    void set_priority(i32 priority)
    {
        if (_control != nullptr)
            _control->priority.store(priority, std::memory_order_relaxed);
    }

    [[nodiscard]] i32 priority() const
    {
        return _control != nullptr ? _control->priority.load(std::memory_order_relaxed) : 0;
    }

    /// Upgrades this transfer to the async tier: it additionally gains the automatic command-list synchronization,
    /// so a list recorded after this call waits on it with no further ceremony.
    ///
    /// Additive — the handle keeps working, and progress and completion keep reporting.
    /// This is what makes a low-priority stream a safe prewarm: guessing wrong is recoverable rather than fatal.
    /// Lists recorded BEFORE the call are unaffected, which is the same rule the handle already states.
    void promote_to_async();

    [[nodiscard]] stream_progress progress() const
    {
        return _control != nullptr ? _control->progress() : stream_progress{};
    }

    /// Whether the transfer has settled, by delivery or by cancellation.
    [[nodiscard]] bool is_settled() const
    {
        return _control != nullptr && _control->completion != nullptr && _control->completion->is_ready();
    }

    /// Whether the transfer delivered everything it was asked to.
    /// False while still running, and false for a cancelled or failed one — is_settled distinguishes those.
    [[nodiscard]] bool is_complete() const { return is_settled() && !_control->completion->has_error(); }

    /// The node completing when this transfer settles: `cc::unit` on success, an `async_error` when cancelled or
    /// failed.
    /// Depend on it to chain work off the transfer without blocking anything.
    [[nodiscard]] cc::shared_async<cc::unit const> completion() const
    {
        return _control != nullptr ? _control->completion : cc::shared_async<cc::unit>();
    }

protected:
    std::shared_ptr<stream_control> _control;
};
} // namespace sg::impl

/// Control handle for one streaming upload, from `ctx.stream.bytes_to_buffer` / `bytes_to_texture`.
/// Dropping it cancels the upload.
class sg::stream_upload_handle final : public sg::impl::stream_handle_base
{
public:
    using stream_handle_base::stream_handle_base;
};

/// Control handle for one streaming download, from `ctx.stream.bytes_from_buffer` / `bytes_from_texture`.
/// Dropping it cancels the download.
/// It also carries the `bytes_future` the bytes land in, so the zero-copy paths are the same ones an async download
/// already uses.
class sg::stream_download_handle final : public sg::impl::stream_handle_base
{
public:
    stream_download_handle() = default;
    stream_download_handle(std::shared_ptr<impl::stream_control> control, bytes_future future)
      : stream_handle_base(cc::move(control)), _future(cc::move(future))
    {
    }

    /// The destination the bytes land in, valid once the transfer completes.
    /// Copyable and independent of this handle: it keeps its own pin, so it outlives a cancelled or dropped handle.
    [[nodiscard]] bytes_future const& future() const { return _future; }

private:
    bytes_future _future;
};
