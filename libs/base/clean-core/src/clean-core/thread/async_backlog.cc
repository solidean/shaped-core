#include "async_backlog.hh"

#include <clean-core/container/ringbuffer.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>

using namespace cc::primitive_defines;

namespace cc::impl
{
struct async_backlog_state
{
    cc::mutex<cc::ringbuffer<async_node_weak>> entries;
};

namespace
{
enum class entry_status
{
    gone,    // freed or settled: nothing left to wait for
    cold,    // alive but unstarted: kept, not waited for
    pending, // started and unsettled
};

[[nodiscard]] entry_status status_of(async_node_ptr const& node)
{
    if (node == nullptr || node->is_ready())
        return entry_status::gone;
    return node->is_cold() ? entry_status::cold : entry_status::pending;
}

/// Compacts `state` down to its live entries, appending the pending ones to `pinned` and every other handle it locked to `released`.
void sweep(async_backlog_state& state, cc::vector<async_node_ptr>& pinned, cc::vector<async_node_ptr>& released)
{
    state.entries.lock(
        [&](cc::ringbuffer<async_node_weak>& entries)
        {
            auto const count = entries.size();
            for (auto i = isize(0); i < count; ++i)
            {
                auto entry = entries.pop_front();
                auto node = entry.lock();
                switch (status_of(node))
                {
                case entry_status::gone:
                    released.push_back(cc::move(node));
                    break;
                case entry_status::cold:
                    entries.push_back(cc::move(entry));
                    released.push_back(cc::move(node));
                    break;
                case entry_status::pending:
                    entries.push_back(cc::move(entry));
                    pinned.push_back(cc::move(node));
                    break;
                }
            }
        });
}
} // namespace
} // namespace cc::impl

cc::async_backlog::async_backlog() : _state(cc::make_shared<impl::async_backlog_state>())
{
}

cc::async_backlog::~async_backlog() = default;

void cc::async_backlog::track_node(async_node_weak node)
{
    // Locked entries are released after the lock: dropping what may be the last handle runs a node's teardown, which
    // may track into this very backlog.
    auto released = cc::vector<async_node_ptr>();
    _state->entries.lock(
        [&](cc::ringbuffer<async_node_weak>& entries)
        {
            // Pruned from the front only, which is where settled work collects; a gap further in waits for settled().
            while (!entries.empty())
            {
                auto front = entries.front().lock();
                if (impl::status_of(front) != impl::entry_status::gone)
                {
                    released.push_back(cc::move(front));
                    break;
                }
                released.push_back(cc::move(front));
                entries.remove_front();
            }
            entries.push_back(cc::move(node));
        });
}

cc::shared_async<cc::unit> cc::async_backlog::settled() const
{
    auto const self = this;
    return settled(cc::span<async_backlog const* const>(&self, 1));
}

cc::shared_async<cc::unit> cc::async_backlog::settled(cc::span<async_backlog const* const> backlogs)
{
    auto states = cc::vector<cc::shared_ptr<impl::async_backlog_state>>();
    states.reserve(backlogs.size());
    for (auto const* const backlog : backlogs)
        states.push_back(backlog->_state);

    return cc::make_async_lazy<cc::unit>(
        [states = cc::move(states),
         pinned = cc::vector<async_node_ptr>()](cc::async_context<cc::unit>& actx) mutable -> cc::async_step_status
        {
            while (true)
            {
                // The round in flight waits for everything it pinned, and holds it strongly so a required node outlives the wait.
                auto waiting = false;
                for (auto const& node : pinned)
                    waiting = !actx.require(node) || waiting; // never short-circuit: every dependency must be registered
                if (waiting)
                    return actx.wait_for_dependencies();

                // The next round is whatever is pending now, which includes work the last round's nodes tracked as they settled.
                // What the sweep drops is released after each lock, for track_node's reason.
                auto released = cc::vector<async_node_ptr>();
                for (auto& node : pinned)
                    released.push_back(cc::move(node));
                pinned.clear();

                for (auto const& state : states)
                    impl::sweep(*state, pinned, released);

                if (pinned.empty())
                    return actx.success(cc::unit{});
            }
        });
}

isize cc::async_backlog::outstanding_count() const
{
    auto released = cc::vector<async_node_ptr>();
    auto const count = _state->entries.lock(
        [&](cc::ringbuffer<async_node_weak>& entries)
        {
            auto pending = isize(0);
            for (auto i = isize(0); i < entries.size(); ++i)
            {
                auto node = entries[i].lock();
                if (impl::status_of(node) == impl::entry_status::pending)
                    ++pending;
                released.push_back(cc::move(node));
            }
            return pending;
        });
    return count;
}
