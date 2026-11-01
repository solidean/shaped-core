#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/thread/async_ambient.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh>

/// Whose context a pending async upload still carries, held by the destination resource rather than by the job.
///
/// A job carries the context of whoever enqueued it, so draining that context waits for the GPU to have the bytes
/// (libs/graphics/shaped-graphics/docs/concepts/threading.md, "Whose work a transfer actor is doing").
/// Once the destination's last handle is gone and no submitted list waits on the job's value, nobody can ever read
/// those bytes, and the context has nothing left to wait for.
/// The job learns that only when the transfer actor next looks at it, which in an unthreaded build is the next pump:
/// so the context would stay held for work nobody can observe.
///
/// Holding it here lets the resource's destructor release it at the moment it becomes unobservable, from whichever
/// thread drops the last handle, while the actor still borrows it for the job's own work until then.
namespace sg::impl
{
/// The context of each pending async upload into one resource, by its completion value on that resource's timeline.
class upload_contexts
{
public:
    /// Holds `ambient` for the upload whose completion value is `value`.
    void hold(u64 value, cc::async_ambient_handle ambient)
    {
        _entries.lock([&](cc::vector<entry>& entries)
                      { entries.push_back({.value = value, .ambient = cc::move(ambient)}); });
    }

    /// A reference to the context of `value`'s upload, empty once it was released.
    [[nodiscard]] cc::async_ambient_handle borrow(u64 value) const
    {
        auto result = _entries.lock(
            [&](cc::vector<entry> const& entries) -> cc::optional<cc::async_ambient_handle>
            {
                for (auto const& e : entries)
                    if (e.value == value)
                        return e.ambient;
                return {};
            });
        if (result.has_value())
            return cc::move(result.value());
        // a default handle captures whatever this thread has installed, so it is emptied rather than returned
        auto empty = cc::async_ambient_handle();
        empty.reset();
        return empty;
    }

    /// Releases the context of `value`'s upload, which has settled or been retired.
    void release(u64 value)
    {
        auto released = cc::vector<entry>();
        _entries.lock(
            [&](cc::vector<entry>& entries)
            {
                for (auto i = isize(0); i < entries.size(); ++i)
                    if (entries[i].value == value)
                    {
                        released.push_back(cc::move(entries[i]));
                        entries.remove_at(i);
                        return;
                    }
            });
        // dropped outside the lock: a release may resume whatever waited for the context to drain
    }

    /// Releases the context of every upload no submitted list waits on: each one above `awaited_value`.
    /// Called when the resource's last handle is gone, after which no new list can come to wait on one.
    void release_unawaited(u64 awaited_value)
    {
        auto released = cc::vector<entry>();
        _entries.lock(
            [&](cc::vector<entry>& entries)
            {
                for (auto i = isize(0); i < entries.size();)
                    if (entries[i].value > awaited_value)
                    {
                        released.push_back(cc::move(entries[i]));
                        entries.remove_at(i);
                    }
                    else
                        ++i;
            });
    }

private:
    struct entry
    {
        u64 value = 0;
        cc::async_ambient_handle ambient;
    };
    mutable cc::mutex<cc::vector<entry>> _entries;
};

/// A pending upload's claim on its context, which its destination's completion group holds in `contexts`.
/// Move-only, and released when destroyed, so every path a job leaves its actor by releases it exactly once.
template <class GroupHandle>
class upload_context_ticket
{
public:
    upload_context_ticket() = default;

    /// Captures the calling thread's context for the upload whose completion value is `value` on `group`.
    upload_context_ticket(GroupHandle group, u64 value) : _group(cc::move(group)), _value(value)
    {
        if (_group != nullptr)
            _group->contexts.hold(_value, cc::async_ambient_handle());
    }

    upload_context_ticket(upload_context_ticket&& rhs) noexcept
      : _group(cc::move(rhs._group)), _value(cc::exchange(rhs._value, 0))
    {
        rhs._group = nullptr;
    }
    upload_context_ticket& operator=(upload_context_ticket&& rhs) noexcept
    {
        if (this != &rhs)
        {
            reset();
            _group = cc::move(rhs._group);
            rhs._group = nullptr;
            _value = cc::exchange(rhs._value, 0);
        }
        return *this;
    }
    upload_context_ticket(upload_context_ticket const&) = delete;
    upload_context_ticket& operator=(upload_context_ticket const&) = delete;
    ~upload_context_ticket() { reset(); }

    /// The context to install around this job's own work; empty once the destination released it.
    /// It must outlive the `cc::async_ambient_install_scope` it is handed to.
    [[nodiscard]] cc::async_ambient_handle get() const
    {
        if (_group == nullptr)
        {
            auto empty = cc::async_ambient_handle();
            empty.reset();
            return empty;
        }
        return _group->contexts.borrow(_value);
    }

    /// Releases the context now, before anything the job settles.
    void reset()
    {
        if (_group != nullptr)
            _group->contexts.release(_value);
        _group = nullptr;
        _value = 0;
    }

private:
    GroupHandle _group;
    u64 _value = 0;
};
} // namespace sg::impl
