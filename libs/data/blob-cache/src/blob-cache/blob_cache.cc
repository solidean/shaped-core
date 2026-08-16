#include <babel-serializer/data/sqlite.hh>
#include <blob-cache/blob_cache.hh>
#include <blob-cache/impl/cache_core.hh>
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>

namespace bcache
{
namespace
{
using namespace impl;

/// The acquire pipeline, as one raw compute frame.
///
/// A frame rather than a chain of make_async_lazy continuations, because the node a caller holds must BE the
/// pipeline: only then does driving the result drive the work, which is what lets the cache impose no scheduler of its own beyond the one the caller's compute already needs.
///
/// State persists across polls — the frame is never moved — so the step counter is the whole state machine.
struct acquire_frame
{
    /// WEAK: a caller may destroy its blob_cache while this is in flight, and the store is then simply skipped.
    /// A strong reference here would keep the actor thread alive waiting on work nobody wants any more.
    std::weak_ptr<cache_core> core;

    cache_key key;
    u64 generation = 0;
    acquire_options options;
    cc::unique_function<cc::shared_async<blob>()> compute;

    i32 step = 0;
    double compute_began = 0;
    cc::shared_async<cc::optional<cache_hit>> lookup;
    cc::shared_async<blob> computed;

    cc::async_step_status operator()(cc::async_context<blob>& ctx)
    {
        switch (step)
        {
        case 0:
            return begin_lookup(ctx);
        case 1:
            return after_lookup(ctx);
        default:
            return after_compute(ctx);
        }
    }

    cc::async_step_status begin_lookup(cc::async_context<blob>& ctx)
    {
        step = 1;

        auto const held = core.lock();
        if (options.bypass_lookup || held == nullptr || !held->has_actor() || held->is_closed.load())
            lookup = cc::make_async_from_value<cc::optional<cache_hit>>({});
        else
        {
            lookup = cc::make_async_manual<cc::optional<cache_hit>>();
            if (!held->actor->enqueue_message(get_request{.key = key, .promise = lookup}))
                lookup = cc::make_async_from_value<cc::optional<cache_hit>>({}); // shutting down: a miss, like any other
        }

        (void)ctx.require(lookup);
        return ctx.wait_for_dependencies();
    }

    cc::async_step_status after_lookup(cc::async_context<blob>& ctx)
    {
        // A failed lookup and an empty one are the same thing here, which is the whole degrade-to-a-miss rule:
        // the fallback is identical, so there is nothing for a caller to branch on.
        if (lookup->has_value())
            if (auto const* hit = lookup->try_value(); hit != nullptr && hit->has_value())
                return finish(ctx, hit->value().data, /* store = */ false);

        if (auto const held = core.lock(); held != nullptr && held->is_closed.load())
            return cancel(ctx);

        step = 2;
        compute_began = steady_now();
        bump(&cache_stats::computes_started);

        computed = compute();
        if (computed.get() == nullptr)
        {
            release();
            return ctx.resolve_to_error(cc::async_error::make_error(cc::any_error(cc::string("the compute callback "
                                                                                             "produced no async"))));
        }

        (void)ctx.require(computed);
        return ctx.wait_for_dependencies();
    }

    cc::async_step_status after_compute(cc::async_context<blob>& ctx)
    {
        // A raw frame does not auto-propagate a dependency's error, so the compute's failure is forwarded by hand.
        // This is the ONE failure a caller sees through acquire: the cache's own never reach here.
        if (auto const* error = computed->try_error(); error != nullptr)
        {
            release();
            return ctx.resolve_to_error(computed->propagate_error());
        }

        auto const* value = computed->try_value();
        if (value == nullptr)
            return cancel(ctx);

        return finish(ctx, *value, /* store = */ true);
    }

    /// The terminal step, and the two orderings in it that are load-bearing.
    ///
    /// The slot is released BEFORE the node resolves: resolving wakes dependents, and a dependent that immediately
    /// re-acquires this key must find the slot gone rather than joining an operation that is already over.
    ///
    /// resolve_to_value is the LAST statement, and takes its argument by value.
    /// Resolving is terminal — it destroys this frame along with every capture in it — so nothing may be touched
    /// afterwards, and the value must not be a reference into anything the frame owns.
    cc::async_step_status finish(cc::async_context<blob>& ctx, blob value, bool store)
    {
        if (store)
            enqueue_store(value);
        release();
        return ctx.resolve_to_value(cc::move(value));
    }

    cc::async_step_status cancel(cc::async_context<blob>& ctx)
    {
        release();
        return ctx.resolve_to_error(cc::async_error::make_cancelled());
    }

    void enqueue_store(blob const& value)
    {
        auto const held = core.lock();
        if (held == nullptr || !held->has_actor() || held->is_closed.load())
            return;

        auto put = options.put;
        if (put.compute_time_secs <= 0)
            put.compute_time_secs = steady_now() - compute_began;

        // Fire and forget: the promise is dropped here, and a failure to store never touches the value we return.
        // That is the mechanical reason a broken cache is slow rather than lossy.
        (void)held->actor->enqueue_message(put_request{.key = key,
                                                       .data = value,
                                                       .options = cc::move(put),
                                                       .promise = cc::make_async_manual<put_result>()});
    }

    void release()
    {
        if (auto const held = core.lock())
            held->flights.release(key, generation);
    }

    void bump(i64 cache_stats::* field) const
    {
        if (auto const held = core.lock())
            held->stats.lock([&](cache_stats& s) { s.*field += 1; });
    }

    double steady_now() const { return cc::current_time_steady_secs(); }
};
} // namespace

// ---- creation ------------------------------------------------------------------------------------

blob_cache::blob_cache()
{
    // The disabled shape: a core with no actor.
    // Every path below already handles that, so there is no second "not opened yet" state anywhere in the class.
    _core = std::make_shared<cache_core>();
    _core->opened = cc::make_async_from_value(cc::unit{});
}

blob_cache::~blob_cache()
{
    this->close();
}

bool blob_cache::is_storage_available()
{
    return babel::sqlite::is_available();
}

cc::unique_ptr<blob_cache> blob_cache::create(cache_config config)
{
    auto cache = cc::make_unique<blob_cache>();
    cache->_core->limits.lock([&](cache_limits& l) { l = config.limits; });
    cache->_core->opened = cc::make_async_manual<cc::unit>();

    auto const unthreaded = config.unthreaded;
    cache->_core->actor = make_cache_actor(unthreaded);

    // The open is a message like any other, so nothing here touches the disk and the handle is usable at once.
    (void)cache->_core->actor->enqueue_message(
        open_request{.config = cc::move(config), .core = cache->_core, .promise = cache->_core->opened});

    return cache;
}

cc::unique_ptr<blob_cache> blob_cache::create_disabled()
{
    return cc::make_unique<blob_cache>();
}

cc::shared_async<cc::unit> blob_cache::opened() const
{
    return _core->opened;
}

// ---- reading and writing -------------------------------------------------------------------------

cc::shared_async<cc::optional<cache_hit>> blob_cache::get(cache_key key)
{
    if (!_core->has_actor() || _core->is_closed.load())
        return cc::make_async_from_value<cc::optional<cache_hit>>({});

    auto promise = cc::make_async_manual<cc::optional<cache_hit>>();
    if (!_core->actor->enqueue_message(get_request{.key = cc::move(key), .promise = promise}))
        return cc::make_async_from_value<cc::optional<cache_hit>>({});
    return promise;
}

cc::shared_async<put_result> blob_cache::put(cache_key key, blob data, put_options options)
{
    if (!_core->has_actor() || _core->is_closed.load())
        return cc::make_async_from_value(put_result{.status = put_status::unavailable});

    auto promise = cc::make_async_manual<put_result>();
    if (!_core->actor->enqueue_message(
            put_request{.key = cc::move(key), .data = cc::move(data), .options = cc::move(options), .promise = promise}))
        return cc::make_async_from_value(put_result{.status = put_status::unavailable});
    return promise;
}

cc::shared_async<blob> blob_cache::acquire(cache_key const& key,
                                           cc::unique_function<cc::shared_async<blob>()> compute,
                                           acquire_options options)
{
    // The generation is minted before the claim, so the frame knows which slot it will release without anything having to reach back into a node that is already running.
    auto const generation = flight_table::next_generation();

    // Built OUTSIDE the lock, and discarded unused on a join.
    // One cold node is the price of keeping the probe atomic while never constructing a compute pipeline under a mutex.
    auto fresh = cc::make_async_lazy<blob>(acquire_frame{.core = _core,
                                                         .key = key,
                                                         .generation = generation,
                                                         .options = cc::move(options),
                                                         .compute = cc::move(compute)});

    auto claim = _core->flights.claim_or_join(key, cc::move(fresh), generation);
    if (!claim.is_owner)
    {
        _core->stats.lock([](cache_stats& s) { s.singleflight_joins += 1; });
        return cc::move(claim.operation);
    }

    // Started here where there is somewhere to run it, and left COLD where there is not — exactly what make_async_scheduled does, widened to the installed default pool.
    //
    // Scheduling unconditionally would assert in a program that drives its own graphs and installs no pool, and
    // leaving it cold unconditionally would make a fire-and-forget acquire silently never happen.
    // A cold operation still resolves the moment anybody drives it, which is what blocking_get does first thing.
    if (cc::async_scheduler::current_or_null() != nullptr || cc::async_scheduler::default_or_null() != nullptr)
        claim.operation->schedule();

    return cc::move(claim.operation);
}

// ---- entries -------------------------------------------------------------------------------------

cc::shared_async<bool> blob_cache::invalidate(cache_key key)
{
    if (!_core->has_actor() || _core->is_closed.load())
        return cc::make_async_from_value(false);

    auto promise = cc::make_async_manual<bool>();
    if (!_core->actor->enqueue_message(invalidate_request{.key = cc::move(key), .promise = promise}))
        return cc::make_async_from_value(false);
    return promise;
}

cc::shared_async<i64> blob_cache::clear(cache_namespace space)
{
    if (!_core->has_actor() || _core->is_closed.load())
        return cc::make_async_from_value(i64(0));

    auto promise = cc::make_async_manual<i64>();
    if (!_core->actor->enqueue_message(clear_request{.space = cc::move(space), .promise = promise}))
        return cc::make_async_from_value(i64(0));
    return promise;
}

// ---- limits and maintenance ----------------------------------------------------------------------

void blob_cache::set_limits(cache_limits limits)
{
    _core->limits.lock([&](cache_limits& l) { l = limits; });
    if (_core->has_actor() && !_core->is_closed.load())
        (void)_core->actor->enqueue_message(limits_request{.limits = limits});
}

cache_limits blob_cache::get_limits() const
{
    return _core->limits.lock([](cache_limits& l) { return l; });
}

cc::shared_async<gc_result> blob_cache::collect_garbage()
{
    if (!_core->has_actor() || _core->is_closed.load())
        return cc::make_async_from_value(gc_result{});

    auto promise = cc::make_async_manual<gc_result>();
    if (!_core->actor->enqueue_message(gc_request{.promise = promise}))
        return cc::make_async_from_value(gc_result{});
    return promise;
}

cc::shared_async<cc::unit> blob_cache::flush()
{
    if (!_core->has_actor() || _core->is_closed.load())
        return cc::make_async_from_value(cc::unit{});

    auto promise = cc::make_async_manual<cc::unit>();
    if (!_core->actor->enqueue_message(flush_request{.promise = promise}))
        return cc::make_async_from_value(cc::unit{});
    return promise;
}

cache_stats blob_cache::get_stats() const
{
    return _core->stats.lock([](cache_stats& s) { return s; });
}

// ---- driving and shutdown ------------------------------------------------------------------------

bool blob_cache::pump()
{
    if (!_core->has_actor())
        return false;
    return _core->actor->process_messages_if_unthreaded();
}

void blob_cache::close()
{
    if (_core == nullptr || _core->is_closed.exchange(true))
        return;

    // Every live operation is resolved as cancelled, not merely dropped: a joiner parked on a compute this shutdown
    // abandoned would otherwise wait for a value nobody is going to produce.
    _core->flights.cancel_all();

    if (!_core->has_actor())
        return;

    (void)_core->actor->enqueue_message(flush_request{.promise = cc::make_async_manual<cc::unit>()});
    (void)_core->actor->enqueue_message(close_request{});

    // shutdown() drains the mailbox before joining, so the flush and close above are always serviced — including in an unthreaded build, where it runs them on this thread.
    _core->actor->shutdown();
}

bool blob_cache::is_closed() const
{
    return _core->is_closed.load();
}
} // namespace bcache
