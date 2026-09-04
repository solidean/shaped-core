#include "cancel.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/io/io_system.hh>

// The control block is where a cancel and a completion meet, and the reason neither has to know about the other.
//
// io_system::cancel is called while the block's lock is held, on purpose.
// It only posts a message and wakes the reactor, so it cannot block on the reactor thread, and holding the lock means
// an operation cannot finish deregistering underneath us while we are naming it.
// The lock order is one-way -- a cancel takes the block and then the mailbox, while a completing operation takes the
// block and has released it again before it touches anything else -- so there is no cycle to deadlock in.

namespace cnet::impl
{
class cancel_state
{
public:
    /// Register `op`, or report that the token was already cancelled and nothing should be waited for.
    [[nodiscard]] bool add(io_system& io, io_operation* op)
    {
        return _data.lock(
            [&](data& d)
            {
                if (d.cancelled)
                    return false;
                d.entries.push_back({.io = &io, .op = op});
                return true;
            });
    }

    void remove(io_operation* op)
    {
        _data.lock(
            [op](data& d)
            {
                for (isize i = 0; i < d.entries.size(); ++i)
                    if (d.entries[i].op == op)
                    {
                        d.entries[i] = d.entries[d.entries.size() - 1];
                        d.entries.remove_back();
                        return;
                    }
            });
    }

    void cancel()
    {
        _data.lock(
            [](data& d)
            {
                d.cancelled = true;
                for (auto const& e : d.entries)
                    e.io->cancel(e.op);
                d.entries.clear();

                // Cascading under this lock is what keeps it safe: a child whose last handle just went away is
                // blocked in detach_from_parent waiting for it, so it is still alive here.
                // The order is one-way -- a parent takes its own lock and then a child's, never the reverse.
                for (auto* const child : d.children)
                    child->cancel();
            });
    }

    /// Register `child`, or report that this token was already cancelled and the child should be too.
    [[nodiscard]] bool add_child(cancel_state* child)
    {
        return _data.lock(
            [child](data& d)
            {
                if (d.cancelled)
                    return false;
                d.children.push_back(child);
                return true;
            });
    }

    void remove_child(cancel_state* child)
    {
        _data.lock(
            [child](data& d)
            {
                for (isize i = 0; i < d.children.size(); ++i)
                    if (d.children[i] == child)
                    {
                        d.children[i] = d.children[d.children.size() - 1];
                        d.children.remove_back();
                        return;
                    }
            });
    }

    void set_parent(cancel_state* parent) { _parent = parent; }

    /// Leave the parent's list, so nothing cascades into an object that is about to be freed.
    void detach_from_parent()
    {
        if (_parent == nullptr)
            return;

        _parent->remove_child(this);
        cancel_state_release(_parent);
        _parent = nullptr;
    }

    [[nodiscard]] bool is_cancelled()
    {
        return _data.lock([](data const& d) { return d.cancelled; });
    }

    void retain() { _references.fetch_add(1); }

    /// True once nobody holds this any more, and the caller is the one that must free it.
    [[nodiscard]] bool release() { return _references.fetch_sub(1) == 1; }

private:
    struct entry
    {
        io_system* io = nullptr;
        io_operation* op = nullptr;
    };

    struct data
    {
        cc::vector<entry> entries;

        /// Held raw rather than retained: a child deregisters itself before it is freed, under this same lock.
        cc::vector<cancel_state*> children;

        bool cancelled = false;
    };

    cc::mutex<data> _data;
    cc::atomic<i32> _references = 1;

    /// Retained, so a child can always reach the parent it has to deregister from.
    cancel_state* _parent = nullptr;
};

void cancel_state_retain(cancel_state* s)
{
    if (s != nullptr)
        s->retain();
}

void cancel_state_release(cancel_state* s)
{
    if (s == nullptr || !s->release())
        return;

    // Leaving the parent's list BEFORE the object goes away is the whole ordering: a cascading cancel holds the
    // parent's lock, so this waits for it and the parent never touches freed memory.
    s->detach_from_parent();
    delete s;
}

// Plain new/delete rather than cc::make_unique: the block outlives every handle to it in an order nobody can name up
// front, which is what an intrusive refcount is for, and cc::unique_ptr deliberately cannot release ownership.
cancel_state* cancel_state_create()
{
    return new cancel_state();
}

cancel_state* cancel_state_create_child(cancel_state* parent)
{
    auto* const child = new cancel_state();
    if (parent == nullptr)
        return child;

    cancel_state_retain(parent);
    child->set_parent(parent);

    // A child of a token that has already been cancelled is born cancelled, which is what makes a late caller fail
    // at once rather than starting work nobody wants.
    if (!parent->add_child(child))
        child->cancel();

    return child;
}

void cancel_registration::attach(cancel_token const& token, io_system& io, io_operation* op)
{
    auto* const state = token.state();
    if (state == nullptr)
        return;

    // Already cancelled, or cancelled between the submit and this line: either way the cancel has to be posted after
    // the operation reached the reactor, which is exactly what this does.
    if (!state->add(io, op))
    {
        io.cancel(op);
        return;
    }

    cancel_state_retain(state);
    _state = state;
    _op = op;
}

void cancel_registration::detach()
{
    if (_state == nullptr)
        return;

    _state->remove(_op);
    cancel_state_release(_state);
    _state = nullptr;
    _op = nullptr;
}
} // namespace cnet::impl

namespace cnet
{
cancel_token cancel_token::create()
{
    auto token = cancel_token();
    token._state = impl::cancel_state_create();
    return token;
}

cancel_token& cancel_token::operator=(cancel_token const& o)
{
    if (this == &o)
        return *this;

    impl::cancel_state_retain(o._state);
    impl::cancel_state_release(_state);
    _state = o._state;
    return *this;
}

cancel_token& cancel_token::operator=(cancel_token&& o) noexcept
{
    if (this == &o)
        return *this;

    impl::cancel_state_release(_state);
    _state = o._state;
    o._state = nullptr;
    return *this;
}

cancel_token cancel_token::create_child() const
{
    auto child = cancel_token();
    child._state = impl::cancel_state_create_child(_state);
    return child;
}

bool cancel_token::is_cancelled() const
{
    return _state != nullptr && _state->is_cancelled();
}

void cancel_token::cancel() const
{
    if (_state != nullptr)
        _state->cancel();
}
} // namespace cnet
