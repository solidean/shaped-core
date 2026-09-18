#pragma once

#include <clean-core/common/utility.hh> // cc::unit
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh> // cc::any_error
#include <clean-core/thread/async.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>
#include <type_traits>

/// Guards a blocking wait that would deadlock the thread performing it.
/// An inline (`cmd.download`) readback cannot be delivered until the recording command list is submitted, so waiting
/// before that would stall the very thread that must submit.
/// Async (`ctx.download`) futures carry no gate: nothing about their delivery depends on the waiting thread.
///
/// Only the deadlock question lives here — cancellation and failure travel on the completion async's error channel.
class sg::bytes_wait_gate
{
public:
    /// Whether blocking on the future could make progress at all.
    [[nodiscard]] bool is_waitable() const { return _submitted.load(std::memory_order_acquire); }

    /// Opens the gate; call once the recording command list has been submitted.
    void mark_submitted() { _submitted.store(true, std::memory_order_release); }

private:
    std::atomic_bool _submitted = false;
};

/// The result of a download command: a pending GPU→CPU transfer of raw bytes.
/// Copyable and movable, and it outlives the command list that recorded it.
/// It holds the destination span, a pin keeping that destination alive until the transfer finishes, and the
/// completion node the backend pushes once those bytes are valid.
/// Await the bytes with bytes(), poll them with try_get_bytes(), or depend on completion() to chain off the transfer.
///
/// Completion rides on `cc::async`, so `completion()` composes a transfer into an async graph without blocking.
/// Cancellation — a dropped recording list, a dropped destination — arrives as `cc::async_error::make_cancelled()`
/// on that node rather than as a future that never completes.
class sg::bytes_future
{
    // The blocking read is kept off the future's own public API: a caller awaits instead.
    friend class context;
    // the typed wrapper forwards its blocking wait to the underlying bytes_future.
    template <class>
    friend class data_future;

    // ctors
public:
    /// An invalid future — not backed by any download.
    bytes_future() = default;

    /// Backs a future by a destination `data` (bytes plus the owner that keeps them alive), completed by `completion`.
    /// The backend fills `data` before pushing that node, and pushes an error to cancel or fail instead.
    /// `data` may be empty.
    /// `gate`, when set, reports whether blocking can make progress at all — see bytes_wait_gate.
    /// This is the single seam a future-provided-destination download reuses.
    bytes_future(cc::pinned_data<byte const> data,
                 cc::shared_async<cc::unit> completion,
                 std::shared_ptr<bytes_wait_gate> gate = nullptr)
      : _data(cc::move(data)), _completion(cc::move(completion)), _gate(cc::move(gate))
    {
    }

    // queries
public:
    /// Whether this future is backed by a real download (vs default-constructed).
    [[nodiscard]] bool is_valid() const { return _completion != nullptr; }

    /// Non-blocking poll: whether the transfer has settled, with bytes or with an error.
    /// A download settles only once its readback actor copy has run, and neither this nor an epoch advance forces that.
    /// Awaiting `bytes()` or `ctx.idle_completion()` waits for it.
    [[nodiscard]] bool is_ready() const { return _completion != nullptr && _completion->is_ready(); }

    /// The node completing when this transfer settles: `cc::unit` on success, an `async_error` when cancelled or failed.
    /// Depend on it to chain work off a transfer without blocking anything; null on an invalid future.
    [[nodiscard]] cc::shared_async<cc::unit const> completion() const { return _completion; }

    /// The bytes, as an async that resolves once they land: `auto const bytes = co_await future.bytes();`.
    /// Fails with the completion's error when the transfer is cancelled or fails; null on an invalid future.
    /// An inline download lands only after its recording list is submitted, so await it after submitting.
    [[nodiscard]] cc::shared_async<cc::pinned_data<byte const>> bytes() const;

    /// The result bytes if delivered (polls), else nullopt — including when the transfer settled on its error channel.
    /// The returned pinned_data keeps the bytes alive on its own, so it stays valid even past this future's lifetime.
    [[nodiscard]] cc::optional<cc::pinned_data<byte const>> try_get_bytes() const;

    // members
private:
    /// Blocks until settled, then returns the bytes.
    /// Returns nullopt if invalid, if the transfer was cancelled or failed, or if blocking cannot make progress
    /// because the recording list is not yet submitted.
    /// Reached only through context::wait_for: a blocking wait is a context-level effect, not a future method.
    [[nodiscard]] cc::optional<cc::pinned_data<byte const>> wait_get_bytes() const;

    cc::pinned_data<byte const> _data;      // destination bytes + owner; valid once the completion holds a value
    cc::shared_async<cc::unit> _completion; // pushed by the backend when the bytes land, or with an error
    std::shared_ptr<bytes_wait_gate> _gate; // null when blocking never depends on the waiting thread
};

namespace sg
{
/// A completion node that is already settled with a value — for empty or synchronous downloads that need no
/// GPU readback.
[[nodiscard]] inline cc::shared_async<cc::unit> make_ready_completion()
{
    return cc::make_async_from_value(cc::unit{});
}
} // namespace sg

/// Strongly-typed view of a bytes_future for a trivially-copyable element type.
/// The byte count must be a multiple of sizeof(T).
template <class T>
class sg::data_future
{
    static_assert(std::is_trivially_copyable_v<T>, "data_future element type must be trivially copyable");

    // The blocking read is kept off the future's own public API: a caller awaits instead.
    friend class context;

public:
    data_future() = default;
    explicit data_future(bytes_future bytes) : _bytes(cc::move(bytes)) {}

    [[nodiscard]] bool is_valid() const { return _bytes.is_valid(); }
    [[nodiscard]] bool is_ready() const { return _bytes.is_ready(); }

    /// The node completing when this transfer settles — see bytes_future::completion.
    [[nodiscard]] cc::shared_async<cc::unit const> completion() const { return _bytes.completion(); }

    /// The typed result, as an async that resolves once it lands: `auto const data = co_await future.data();`.
    /// Fails when the transfer is cancelled or fails, or when the byte count is not a multiple of sizeof(T); null on
    /// an invalid future.
    [[nodiscard]] cc::shared_async<cc::pinned_data<T const>> data() const
    {
        auto bytes = _bytes.bytes();
        if (bytes == nullptr)
            return {};
        return cc::make_async_lazy<cc::pinned_data<T const>>(
            [](cc::async_context<cc::pinned_data<T const>>& ctx, cc::pinned_data<byte const> received)
            {
                auto typed = received.template try_reinterpret_as<T const>();
                if (!typed.has_value())
                    return ctx.error(cc::any_error("the downloaded byte count is not a multiple of the element size"));
                return ctx.success(cc::move(typed.value()));
            },
            cc::move(bytes));
    }

    /// The typed result if delivered (polls).
    /// Yields nullopt when the byte count is not a multiple of sizeof(T).
    [[nodiscard]] cc::optional<cc::pinned_data<T const>> try_get_data() const
    {
        auto const bytes = _bytes.try_get_bytes();
        if (!bytes.has_value())
            return {};
        return bytes.value().template try_reinterpret_as<T const>();
    }

private:
    /// Blocks until settled, then returns the typed result.
    /// Reached only through context::wait_for.
    [[nodiscard]] cc::optional<cc::pinned_data<T const>> wait_get_data() const
    {
        auto const bytes = _bytes.wait_get_bytes();
        if (!bytes.has_value())
            return {};
        return bytes.value().template try_reinterpret_as<T const>();
    }

    bytes_future _bytes;
};
