#include "async_backlog.hh"

#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/ringbuffer.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>

#include <cstdio> // stderr, for the same reason every other report path here uses it

using namespace cc::primitive_defines;

namespace cc::impl
{
namespace
{
/// A ring this small is compacted whenever it fills, and never below it.
constexpr isize min_compact_size = 32;

/// A compaction visiting at least this many entries is recorded as a scope.
constexpr isize compact_scope_threshold = 1024;
} // namespace

struct async_backlog_entries
{
    cc::ringbuffer<async_node_weak> nodes;

    /// The size at which track_node compacts the whole ring: twice what survived the last compaction.
    isize compact_at = min_compact_size;
};

struct async_backlog_state
{
    cc::mutex<async_backlog_entries> entries;
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

/// Drops every gone entry, appending the pending nodes to `pinned` when given and every other handle it locked to `released`.
///
/// Must be called under the entries' lock, and `released` dropped only after it: releasing what may be the last handle
/// runs a node's teardown, which may track into this very backlog.
/// O(entries) under that lock, so a backlog holding a very large number of live entries stalls every tracking thread
/// for the duration — the scope is what makes that visible as frame stutter in a profile.
void compact(async_backlog_entries& entries, cc::vector<async_node_ptr>* pinned, cc::vector<async_node_ptr>& released)
{
    auto const count = entries.nodes.size();
    CC_RECORD_SCOPE_IF(count >= compact_scope_threshold, "cc.async_backlog.compact");

    for (auto i = isize(0); i < count; ++i)
    {
        auto entry = entries.nodes.pop_front();
        auto node = entry.lock();
        auto const status = status_of(node);
        if (status != entry_status::gone)
            entries.nodes.push_back(cc::move(entry));
        if (status == entry_status::pending && pinned != nullptr)
            pinned->push_back(cc::move(node));
        else
            released.push_back(cc::move(node));
    }

    entries.compact_at = cc::max(min_compact_size, 2 * entries.nodes.size());
}
} // namespace
} // namespace cc::impl

namespace
{
/// Every live backlog, so a report can find them all.
///
/// A mutex rather than a lock-free list: construction and destruction are rare by construction — a backlog belongs
/// to a component, not to a piece of work — and nothing on the tracking path touches this.
struct backlog_registry
{
    cc::async_backlog* head = nullptr;
};

cc::mutex<backlog_registry>& backlogs()
{
    // Function-local, so a backlog constructed during dynamic initialization still finds a live registry.
    static cc::mutex<backlog_registry> registry;
    return registry;
}
} // namespace

cc::async_backlog::async_backlog(cc::string_view name)
  : _name(name), _state(cc::make_shared<impl::async_backlog_state>())
{
    backlogs().lock(
        [&](backlog_registry& r)
        {
            _registry_next = r.head;
            if (r.head != nullptr)
                r.head->_registry_prev = this;
            r.head = this;
        });
}

cc::async_backlog::~async_backlog()
{
    backlogs().lock(
        [&](backlog_registry& r)
        {
            if (_registry_prev != nullptr)
                _registry_prev->_registry_next = _registry_next;
            else
                r.head = _registry_next;

            if (_registry_next != nullptr)
                _registry_next->_registry_prev = _registry_prev;
        });
}

bool cc::try_for_each_async_backlog(cc::function_ref<void(cc::async_backlog const&)> f)
{
    // try rather than lock: this runs from a report about a program that has already gone wrong, and a thread that
    // died mid-construction still holds the registry.
    return backlogs().try_lock(
        [&](backlog_registry& r)
        {
            for (auto const* b = r.head; b != nullptr; b = b->_registry_next)
                f(*b);
        });
}

void cc::async_backlog::track_node(async_node_weak node)
{
    // Compacting once the ring doubles past what last survived keeps it within twice its live entries, at O(1)
    // amortised per track, whether or not anyone ever calls settled().
    auto released = cc::vector<async_node_ptr>();
    _state->entries.lock(
        [&](impl::async_backlog_entries& entries)
        {
            entries.nodes.push_back(cc::move(node));
            if (entries.nodes.size() >= entries.compact_at)
                impl::compact(entries, nullptr, released);
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

    // The first round is swept here rather than when the node is awaited, so nothing pending hands back a resolved node.
    // Blocking on that needs no scheduler, which is what lets a teardown with none left call this at all.
    auto pinned = cc::vector<async_node_ptr>();
    {
        auto released = cc::vector<async_node_ptr>();
        for (auto const& state : states)
            state->entries.lock([&](impl::async_backlog_entries& entries) { impl::compact(entries, &pinned, released); });
    }
    if (pinned.empty())
        return cc::make_async_from_value(cc::unit{});

    return cc::make_async_lazy<cc::unit>(
        [states = cc::move(states),
         pinned = cc::move(pinned)](cc::async_context<cc::unit>& actx) mutable -> cc::async_step_status
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
                // What the sweep drops is released after each lock, for compact's reason.
                auto released = cc::vector<async_node_ptr>();
                for (auto& node : pinned)
                    released.push_back(cc::move(node));
                pinned.clear();

                for (auto const& state : states)
                    state->entries.lock([&](impl::async_backlog_entries& entries)
                                        { impl::compact(entries, &pinned, released); });

                if (pinned.empty())
                    return actx.success(cc::unit{});
            }
        });
}

isize cc::async_backlog::outstanding_count() const
{
    auto released = cc::vector<async_node_ptr>();
    auto const count = _state->entries.lock(
        [&](impl::async_backlog_entries& entries)
        {
            auto pending = isize(0);
            for (auto i = isize(0); i < entries.nodes.size(); ++i)
            {
                auto node = entries.nodes[i].lock();
                if (impl::status_of(node) == impl::entry_status::pending)
                    ++pending;
                released.push_back(cc::move(node));
            }
            return pending;
        });
    return count;
}

isize cc::async_backlog::tracked_count() const
{
    return _state->entries.lock([](impl::async_backlog_entries const& entries) { return entries.nodes.size(); });
}

void cc::report_async_backlogs(char const* reason) noexcept
{
    std::fputs("\nwork still in flight", stderr);
    if (reason != nullptr)
    {
        std::fputs(" (", stderr);
        std::fputs(reason, stderr);
        std::fputc(')', stderr);
    }
    std::fputc('\n', stderr);

    auto any = false;
    auto const read = cc::try_for_each_async_backlog(
        [&](cc::async_backlog const& b)
        {
            auto const outstanding = b.outstanding_count();
            if (outstanding <= 0)
                return;

            any = true;
            std::fputs("  backlog ", stderr);

            auto const name = b.name();
            if (name.empty())
                std::fputs("<unnamed>", stderr);
            else
                std::fwrite(name.data(), 1, size_t(name.size()), stderr);

            char buffer[64] = {};
            auto const written = cc::format_to(cc::span<char>(buffer), ": {} outstanding", outstanding);
            std::fwrite(buffer, 1, size_t(written), stderr);
            std::fputc('\n', stderr);
        });

    if (!read)
        std::fputs("  <not read; the backlog registry is busy>\n", stderr);
    else if (!any)
        std::fputs("  <no tracked work outstanding; work awaited in the ordinary way is not tracked>\n", stderr);

    std::fflush(stderr);
}
