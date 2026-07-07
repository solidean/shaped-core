#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>
#include <memory>
#include <type_traits>

namespace sg
{
/// The pollable completion handle behind a bytes_future. Abstract — a backend subclasses it to track
/// its own readback mechanism (fence values, ring positions, ...). A download's data is valid once
/// the waiter reports ready.
class bytes_waiter
{
public:
    virtual ~bytes_waiter();

    /// Whether the download has completed and its destination bytes are valid.
    [[nodiscard]] bool is_ready() const { return _is_ready.load(std::memory_order_acquire); }

    /// Checks completion, possibly advancing internal state to make progress. Returns is_ready().
    [[nodiscard]] virtual bool poll_ready() { return is_ready(); }

    /// Blocks until ready. Returns false when blocking cannot make progress (e.g. the recording
    /// command list has not been submitted yet, so waiting would stall the thread that must submit).
    [[nodiscard]] virtual bool wait() = 0;

    /// Backend seam: marks the download complete and wakes any blocked waiters. Called once the
    /// destination bytes are valid.
    void mark_ready()
    {
        _is_ready.store(true, std::memory_order_release);
        _is_ready.notify_all();
    }

protected:
    /// Set true once the destination bytes are valid; read on the fast path, waited on in wait().
    std::atomic_bool _is_ready = false;
};

/// A bytes_waiter that is ready on construction — for empty or synchronous downloads that need no
/// GPU readback.
class ready_bytes_waiter final : public bytes_waiter
{
public:
    ready_bytes_waiter() { _is_ready.store(true, std::memory_order_release); }

    [[nodiscard]] bool wait() override { return true; }
};

/// The result of a download command: a pending GPU→CPU transfer of raw bytes. Copyable and movable.
/// Holds the destination span, a pin that keeps that destination alive until the transfer finishes,
/// and the waiter that tracks completion. Read the bytes with try_get_bytes() once ready.
class bytes_future
{
    // ctx.wait_for(future) reaches the blocking wait — kept off the future's own public API.
    friend class context;
    // the typed wrapper forwards its blocking wait to the underlying bytes_future.
    template <class>
    friend class data_future;

    // ctors
public:
    /// An invalid future — not backed by any download.
    bytes_future() = default;

    /// Backs a future by a destination `data` (bytes plus the owner that keeps them alive), with
    /// completion tracked by `waiter`. The backend fills `data` before signaling the waiter ready.
    /// `data` may be empty. This is the single seam a future-provided-destination download reuses.
    bytes_future(cc::pinned_data<cc::byte const> data, std::shared_ptr<bytes_waiter> waiter)
      : _data(cc::move(data)), _waiter(cc::move(waiter))
    {
    }

    // queries
public:
    /// Whether this future is backed by a real download (vs default-constructed).
    [[nodiscard]] bool is_valid() const { return _waiter != nullptr; }

    /// Non-blocking poll: whether the bytes are ready. A download is ready only once its actor copy
    /// has run — neither this nor an epoch advance forces that; ctx.wait_for(future) does.
    [[nodiscard]] bool is_ready() const { return _waiter != nullptr && _waiter->poll_ready(); }

    /// The result bytes if ready (polls), else nullopt. The returned pinned_data keeps the bytes alive
    /// on its own, so it stays valid even past this future's lifetime. To block until delivered, use
    /// ctx.wait_for(future).
    [[nodiscard]] cc::optional<cc::pinned_data<cc::byte const>> try_get_bytes() const;

    // members
private:
    /// Blocks until ready, then returns the bytes. Returns nullopt if invalid or if blocking cannot
    /// make progress (the recording list is not yet submitted, or the download was cancelled). Reached
    /// only through context::wait_for — a blocking wait is a context-level effect, not a future method.
    [[nodiscard]] cc::optional<cc::pinned_data<cc::byte const>> wait_get_bytes() const;

    cc::pinned_data<cc::byte const> _data; // destination bytes + owner; valid once the waiter is ready
    std::shared_ptr<bytes_waiter> _waiter;
};

/// Strongly-typed view of a bytes_future for a trivially-copyable element type. The byte count must
/// be a multiple of sizeof(T).
template <class T>
class data_future
{
    static_assert(std::is_trivially_copyable_v<T>, "data_future element type must be trivially copyable");

    // ctx.wait_for(future) reaches the blocking wait — kept off the future's own public API.
    friend class context;

public:
    data_future() = default;
    explicit data_future(bytes_future bytes) : _bytes(cc::move(bytes)) {}

    [[nodiscard]] bool is_valid() const { return _bytes.is_valid(); }
    [[nodiscard]] bool is_ready() const { return _bytes.is_ready(); }

    /// The typed result if ready (polls). Yields nullopt when the byte count is not a multiple of
    /// sizeof(T). To block until delivered, use ctx.wait_for(future).
    [[nodiscard]] cc::optional<cc::pinned_data<T const>> try_get_data() const
    {
        auto const bytes = _bytes.try_get_bytes();
        if (!bytes.has_value())
            return {};
        return bytes.value().template try_reinterpret_as<T const>();
    }

private:
    /// Blocks until ready, then returns the typed result. Reached only through context::wait_for.
    [[nodiscard]] cc::optional<cc::pinned_data<T const>> wait_get_data() const
    {
        auto const bytes = _bytes.wait_get_bytes();
        if (!bytes.has_value())
            return {};
        return bytes.value().template try_reinterpret_as<T const>();
    }

    bytes_future _bytes;
};
} // namespace sg
