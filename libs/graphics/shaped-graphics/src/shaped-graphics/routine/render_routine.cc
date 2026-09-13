#include <clean-core/common/profiling.hh>
#include <shaped-graphics/command_list/command_list.hh> // cmd.context()
#include <shaped-graphics/routine/reload_generation.hh>
#include <shaped-graphics/routine/render_routine_base.hh>
#include <shaped-graphics/routine/routine_registry.hh>

namespace sg
{
// By value because an OVERRIDE is a coroutine, whose reference parameter dangles across its first suspend.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
cc::shared_async<cc::unit> render_routine_base::init_once(routine_init_scope)
{
    // NOT a coroutine: a routine that overrides neither phase would otherwise pay a frame allocation per phase to do
    // nothing at all.
    return cc::make_async_from_value<cc::unit>({});
}

// NOLINTNEXTLINE(performance-unnecessary-value-param) — see init_once
cc::shared_async<cc::unit> render_routine_base::init(routine_init_scope)
{
    return cc::make_async_from_value<cc::unit>({});
}

void render_routine_base::cancel_init_for_reload(init_state& s)
{
    // init_once is never cancelled: it is defined as happening once, and restarting it would either lose work
    // permanently or make the name a lie.
    if (s.in_flight == nullptr || s.in_flight_is_once)
        return;

    if (s.run != nullptr)
        s.run->cancelled = true;
    s.in_flight = {};
    s.run = {};
    s.ready_generation = {};
}

void render_routine_base::cancel_init_for_reload_if_stale(u64 generation)
{
    _init.lock(
        [&](init_state& s)
        {
            if (s.run != nullptr && s.run->generation != generation)
                cancel_init_for_reload(s);
        });
}

bool render_routine_base::needs_init(u64 generation)
{
    return _init.lock([&](init_state& s)
                      { return s.in_flight != nullptr || (!_failed && s.ready_generation != generation); });
}

bool render_routine_base::advance_init(routine_registry& registry, context& ctx, u64 generation)
{
    return _init.lock(
        [&](init_state& s)
        {
            if (s.in_flight != nullptr)
            {
                if (!s.in_flight->is_ready())
                    return false; // still running, on whichever worker has it

                auto const settled_once = s.in_flight_is_once;
                auto const* const value = s.in_flight->try_value();
                auto const was_cancelled
                    = s.in_flight->try_error() != nullptr && s.in_flight->try_error()->is_cancelled();
                s.in_flight = {};
                s.run = {};

                if (value == nullptr)
                {
                    // A CANCELLED phase is not a failed one: the reload that cancelled it starts a fresh run, and
                    // treating the two alike would make every reload look like a broken shader.
                    if (!was_cancelled)
                        _failed = true;
                    return false;
                }

                if (settled_once)
                {
                    s.once_done = true;
                    return false; // init is next, and the tick comes back for it
                }

                // Published here: everything the phase wrote becomes visible to a reader through the readiness this
                // sets, which is what replaces holding the routine's lock across an init that can no longer hold one.
                s.ready_generation = generation;
                return true;
            }

            if (s.ready_generation == generation && !_failed)
                return false; // up to date

            // A fresh generation is a fresh verdict, including for a routine that failed at the last one.
            if (s.ready_generation != generation)
                _failed = false;
            if (_failed)
                return false; // failed at THIS generation; only a reload clears it

            s.run = std::make_shared<impl::routine_run>(
                impl::routine_run{.ctx = &ctx, .registry = &registry, .generation = generation});

            s.in_flight_is_once = !s.once_done;
            auto scope = routine_init_scope(s.run);
            s.in_flight = cc::async_start(s.in_flight_is_once ? init_once(scope) : init(scope));
            return false;
        });
}

routine_readiness render_routine_base::own_readiness_locked(init_state const& s)
{
    if (_failed)
        return routine_readiness::failed;
    return s.ready_generation == current_generation() ? routine_readiness::ready : routine_readiness::pending;
}

routine_readiness render_routine_base::own_readiness()
{
    return _init.lock([this](init_state& s) { return own_readiness_locked(s); });
}

bool render_routine_base::is_initialized()
{
    return own_readiness() == routine_readiness::ready;
}

void render_routine_base::note_pending_at(u64 tick)
{
    // The FIRST tick it was pending at is what the age is measured from, so a routine that has been stuck since the
    // start does not look freshly pending on every sweep.
    u64 expected = 0;
    (void)_first_pending_tick.compare_exchange_strong(expected, tick, cc::memory_order_relaxed);
}

void render_routine_base::clear_pending_mark()
{
    // Came up (or failed), so the next time it goes pending is a fresh span and a fresh verdict.
    _first_pending_tick.store(0, cc::memory_order_relaxed);
    _warned_pending.store(false, cc::memory_order_relaxed);
}

void render_routine_base::fail_init()
{
    _failed = true;
}

u64 render_routine_base::current_generation()
{
    return sg::reload_generation();
}
} // namespace sg
