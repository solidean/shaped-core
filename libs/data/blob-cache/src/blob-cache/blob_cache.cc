#include <babel-serializer/data/sqlite.hh>
#include <blob-cache/blob_cache.hh>
#include <blob-cache/impl/cache_core.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/async_scope.hh>
#include <clean-core/thread/async_coroutine.hh> // including it is what makes acquire_pipeline a coroutine

namespace bcache
{
namespace
{
using namespace impl;

/// Releases the singleflight slot when the pipeline ends, however it ends.
///
/// Destruction order is what makes the timing correct rather than something every exit has to remember: the node's
/// frame destroys the coroutine — running this — and only then resolves the node.
/// A dependent that re-acquires the key the instant it wakes therefore finds the slot gone, instead of joining an
/// operation that is already over.
struct flight_release
{
    /// WEAK: a caller may destroy its blob_cache while this is in flight, and the store is then simply skipped.
    /// A strong reference here would keep the actor thread alive waiting on work nobody wants any more.
    std::weak_ptr<cache_core> core;
    cache_key key;
    u64 generation = 0;

    ~flight_release()
    {
        if (auto const held = core.lock())
            held->flights.release(key, generation);
    }
};

void bump(std::weak_ptr<cache_core> const& core, i64 cache_stats::* field)
{
    if (auto const held = core.lock())
        held->stats.lock([&](cache_stats& s) { s.*field += 1; });

    // Outside the lock, and deliberately: a dead core means nobody can read the struct any more, but the event still
    // belongs in the stream of the run that produced it.
    impl::record_stat(field, 1);
}

/// The lookup half, as a node the pipeline can park on.
/// A cache that cannot answer — disabled, closing, or asked to skip — reports a miss rather than an error, which is
/// the degrade-to-a-miss rule at its source.
cc::shared_async<cc::optional<cache_hit>> begin_lookup(std::weak_ptr<cache_core> const& core,
                                                       cache_key const& key,
                                                       bool bypass_lookup)
{
    auto const held = core.lock();
    if (bypass_lookup || held == nullptr || !held->has_actor() || held->is_closed.load())
        return cc::make_async_from_value<cc::optional<cache_hit>>({});

    auto lookup = cc::make_async_manual<cc::optional<cache_hit>>();
    if (!held->actor->enqueue_message(get_request{.key = key, .promise = lookup}))
        return cc::make_async_from_value<cc::optional<cache_hit>>({}); // shutting down: a miss, like any other
    return lookup;
}

void enqueue_store(std::weak_ptr<cache_core> const& core,
                   cache_key const& key,
                   put_options options,
                   blob const& value,
                   double compute_began)
{
    auto const held = core.lock();
    if (held == nullptr || !held->has_actor() || held->is_closed.load())
        return;

    // Only where the caller said nothing: a measurement it supplied is the one it meant, and ours would include
    // however long the compute sat queued before anything drove it.
    if (!options.compute_time_secs.has_value())
        options.compute_time_secs = cc::current_time_steady_secs() - compute_began;

    // Fire and forget: the promise is dropped here, and a failure to store never touches the value we return.
    // That is the mechanical reason a broken cache is slow rather than lossy.
    (void)held->actor->enqueue_message(put_request{.key = key,
                                                   .data = value,
                                                   .options = cc::move(options),
                                                   .promise = cc::make_async_manual<put_result>()});
}

/// The acquire pipeline: lookup, then compute on a miss, then store.
///
/// COLD, like make_async_lazy — the node a caller holds must BE the pipeline, so that driving the result drives the
/// work and the cache imposes no scheduler of its own beyond the one the caller's compute already needs.
///
/// Every await is `async_settled`, never a plain one.
/// A plain await short-circuits on a failed dependency, and the cache's own failures must never reach the caller:
/// a lookup that failed is a miss, and the compute's error is the single failure forwarded on purpose.
///
/// Parameters are by value because a coroutine captures them by declared type, so a reference would dangle across the first suspend.
cc::shared_async<blob> acquire_pipeline(std::weak_ptr<cache_core> core,
                                        cache_key key,
                                        u64 generation,
                                        acquire_options options,
                                        cc::unique_function<cc::shared_async<blob>()> compute)
{
    auto const release = flight_release{.core = core, .key = key, .generation = generation};

    auto const lookup = begin_lookup(core, key, options.bypass_lookup);
    co_await cc::async_settled(lookup);

    // A failed lookup and an empty one are the same thing here: the fallback is identical, so there is nothing for a
    // caller to branch on.
    if (auto const* hit = lookup->try_value(); hit != nullptr && hit->has_value())
        co_return hit->value().data;

    if (auto const held = core.lock(); held != nullptr && held->is_closed.load())
        co_await cc::async_fail(cc::async_error::make_cancelled());

    auto const compute_began = cc::current_time_steady_secs();
    bump(core, &cache_stats::computes_started);

    auto const computed = compute();
    if (computed.get() == nullptr)
        co_await cc::async_fail(cc::string("the compute callback produced no async"));

    co_await cc::async_settled(computed);

    // The ONE failure a caller sees through acquire; the cache's own never reach here.
    if (auto const* error = computed->try_error(); error != nullptr)
        co_await cc::async_fail(computed->propagate_error());

    auto const* const value = computed->try_value();
    if (value == nullptr)
        co_await cc::async_fail(cc::async_error::make_cancelled());

    enqueue_store(core, key, cc::move(options.put), *value, compute_began);
    co_return *value;
}
} // namespace

namespace impl
{
void record_stat(i64 cache_stats::* field, i64 n)
{
    if (field == &cache_stats::hits)
        CC_RECORD_ACCUM("bcache.hits", cc::rec::unit_count, n);
    else if (field == &cache_stats::misses)
        CC_RECORD_ACCUM("bcache.misses", cc::rec::unit_count, n);
    else if (field == &cache_stats::expired_as_miss)
        CC_RECORD_ACCUM("bcache.expired_as_miss", cc::rec::unit_count, n);
    else if (field == &cache_stats::puts_stored)
        CC_RECORD_ACCUM("bcache.puts_stored", cc::rec::unit_count, n);
    else if (field == &cache_stats::puts_deduplicated)
        CC_RECORD_ACCUM("bcache.puts_deduplicated", cc::rec::unit_count, n);
    else if (field == &cache_stats::puts_lost_race)
        CC_RECORD_ACCUM("bcache.puts_lost_race", cc::rec::unit_count, n);
    else if (field == &cache_stats::put_failures)
        CC_RECORD_ACCUM("bcache.put_failures", cc::rec::unit_count, n);
    else if (field == &cache_stats::computes_started)
        CC_RECORD_ACCUM("bcache.computes_started", cc::rec::unit_count, n);
    else if (field == &cache_stats::singleflight_joins)
        CC_RECORD_ACCUM("bcache.singleflight_joins", cc::rec::unit_count, n);
    else if (field == &cache_stats::access_rows_written)
        CC_RECORD_ACCUM("bcache.access_rows_written", cc::rec::unit_count, n);
    else if (field == &cache_stats::entries_evicted)
        CC_RECORD_ACCUM("bcache.entries_evicted", cc::rec::unit_count, n);
    else if (field == &cache_stats::bytes_reclaimed)
        CC_RECORD_ACCUM("bcache.bytes_reclaimed", cc::rec::unit_bytes, n);
    else
        CC_ASSERT(false, "this cache_stats counter has no recorded name — add one in record_stat");
}
} // namespace impl

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
    // Around the code that BUILDS the pipeline rather than inside the coroutine, so the span covers the singleflight
    // decision too — a JOINER never enters the coroutine at all, and is still part of this acquire.
    //
    // It rides cc::async's ambient chain, so the node created below inherits it and carries it to whichever worker
    // ends up running the lookup, the compute and the store — which is what makes one acquire read as one operation.
    CC_RECORD_ASYNC_SCOPE("bcache.acquire");

    // The generation is minted before the claim, so the frame knows which slot it will release without anything having to reach back into a node that is already running.
    auto const generation = flight_table::next_generation();

    // Built OUTSIDE the lock, and discarded unused on a join.
    // One cold node is the price of keeping the probe atomic while never constructing a compute pipeline under a mutex.
    auto fresh = acquire_pipeline(_core, key, generation, cc::move(options), cc::move(compute));

    auto claim = _core->flights.claim_or_join(key, cc::move(fresh), generation);
    if (!claim.is_owner)
    {
        // Through the same adder as every other counter: this acquire turned out to BE the one already running,
        // which is what singleflight exists to make true.
        bump(_core, &cache_stats::singleflight_joins);
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
