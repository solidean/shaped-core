#include "connection_pool.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/io/io_system.hh>

// A list per origin, and nothing cleverer.
//
// A handful of origins with a handful of connections each is what a client sees, so a linear scan is faster than a
// hash and much easier to be sure of.
// If that stops being true, this is one file.

namespace cnet
{
namespace
{
struct idle_connection
{
    cc::shared_ptr<stream_connection> connection;

    /// When it stops being offered, on the io_system's clock.
    i64 expires_ns = 0;
};

struct origin_entry
{
    cc::string origin;
    cc::vector<idle_connection> idle;
};
} // namespace

struct connection_pool::state
{
    io_system& io;
    description desc;
    cc::mutex<cc::vector<origin_entry>> origins;

    state(io_system& s, description const& d) : io(s), desc(d) {}
};

connection_pool::connection_pool(io_system& io) : connection_pool(io, description())
{
}

connection_pool::connection_pool(io_system& io, description const& desc) : _state(cc::make_unique<state>(io, desc))
{
}

connection_pool::~connection_pool() = default;

cc::shared_ptr<stream_connection> connection_pool::try_take(cc::string_view origin)
{
    auto const now = _state->io.time_source().now_ns();

    return _state->origins.lock(
        [&](cc::vector<origin_entry>& all) -> cc::shared_ptr<stream_connection>
        {
            for (auto& entry : all)
            {
                if (cc::string_view(entry.origin) != origin)
                    continue;

                // Newest first: the one used most recently is the one a server is least likely to have given up on.
                while (!entry.idle.empty())
                {
                    auto candidate = cc::move(entry.idle[entry.idle.size() - 1]);
                    entry.idle.remove_back();

                    if (candidate.expires_ns <= now || !candidate.connection.is_valid()
                        || !candidate.connection->is_open())
                    {
                        if (candidate.connection.is_valid())
                            candidate.connection->close();
                        continue;
                    }

                    return candidate.connection;
                }
                return {};
            }
            return {};
        });
}

void connection_pool::give_back(cc::string_view origin, cc::shared_ptr<stream_connection> connection, bool reusable)
{
    if (!connection.is_valid())
        return;

    if (!reusable || !connection->is_open())
    {
        connection->close();
        return;
    }

    auto const expires_ns = _state->io.time_source().now_ns() + _state->desc.idle_timeout_ms * 1000 * 1000;

    auto evicted = _state->origins.lock(
        [&](cc::vector<origin_entry>& all) -> cc::shared_ptr<stream_connection>
        {
            for (auto& entry : all)
            {
                if (cc::string_view(entry.origin) != origin)
                    continue;

                entry.idle.push_back({.connection = cc::move(connection), .expires_ns = expires_ns});

                if (entry.idle.size() <= _state->desc.max_idle_per_origin)
                    return {};

                // Over the cap: the oldest goes, since it is the likeliest to be dead already.
                auto oldest = cc::move(entry.idle[0]);
                for (isize i = 1; i < entry.idle.size(); ++i)
                    entry.idle[i - 1] = cc::move(entry.idle[i]);
                entry.idle.remove_back();
                return oldest.connection;
            }

            auto entry = origin_entry();
            entry.origin = cc::string(origin);
            entry.idle.push_back({.connection = cc::move(connection), .expires_ns = expires_ns});
            all.push_back(cc::move(entry));
            return {};
        });

    // Closed outside the lock: it can run a completion, and nothing else should be waiting on this lock while it
    // does.
    if (evicted.is_valid())
        evicted->close();
}

void connection_pool::clear()
{
    auto held = _state->origins.lock(
        [](cc::vector<origin_entry>& all)
        {
            auto taken = cc::vector<cc::shared_ptr<stream_connection>>();
            for (auto& entry : all)
                for (auto& idle : entry.idle)
                    taken.push_back(cc::move(idle.connection));
            all.clear();
            return taken;
        });

    for (auto& connection : held)
        if (connection.is_valid())
            connection->close();
}

isize connection_pool::idle_count() const
{
    return _state->origins.lock(
        [](cc::vector<origin_entry> const& all)
        {
            auto count = isize(0);
            for (auto const& entry : all)
                count += entry.idle.size();
            return count;
        });
}
} // namespace cnet
