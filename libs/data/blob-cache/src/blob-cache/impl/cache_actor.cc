#include <babel-serializer/data/sqlite.hh>
#include <blob-cache/impl/cache_actor.hh>
#include <blob-cache/impl/cache_core.hh>
#include <blob-cache/impl/cache_gc.hh>
#include <blob-cache/impl/cache_io.hh>
#include <blob-cache/impl/cache_schema.hh>
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>

namespace bcache::impl
{
namespace
{
namespace sql = babel::sqlite;

/// The timing policy, copied out of cache_config at open so the actor never reads a caller's struct again.
struct timing_policy
{
    double access_epoch_secs = 300;
    i64 access_flush_threshold = 256;
    double access_flush_interval_secs = 5;
    double gc_interval_secs = 60;
    double default_compute_secs = 0.01;
    double half_life_secs = 3600;
    bool verify_on_read = false;
};

/// The connection owner.
/// Every member here is touched only on the actor thread, so none of them is synchronized.
class cache_actor_impl final
  : public cc::threaded_actor_impl<open_request, get_request, put_request, invalidate_request, clear_request, limits_request, gc_request, flush_request, close_request>
{
public:
    cc::string_view actor_name() const noexcept override { return "bcache"; }

    // ---- messages --------------------------------------------------------------------------------

    void on_message(open_request msg) override
    {
        _core = cc::move(msg.core);
        _limits = normalize(msg.config.limits);

        // Clamped once, here, and read only from _timing afterwards.
        // half_life_for takes the CLAMPED epoch: derived from the raw one it would disagree with the quantum the
        // access buffer actually uses, for exactly the sub-second setting the clamp exists to catch.
        auto const epoch_secs = cc::max(msg.config.access_epoch_secs, 1.0);
        _timing = {.access_epoch_secs = epoch_secs,
                   .access_flush_threshold = cc::max(msg.config.access_flush_threshold, i64(1)),
                   .access_flush_interval_secs = msg.config.access_flush_interval_secs,
                   .gc_interval_secs = msg.config.gc_interval_secs,
                   .default_compute_secs = cc::max(msg.config.default_compute_time_secs, 0.0),
                   .half_life_secs = half_life_for(epoch_secs),
                   .verify_on_read = msg.config.verify_on_read};
        _wall = cc::move(msg.config.wall_clock);
        _steady = cc::move(msg.config.steady_clock);
        _on_error = cc::move(msg.config.on_storage_error);

        auto result = open_database(msg.config.path);
        if (result.has_error())
        {
            degrade(cc::move(result.error()));
            msg.promise->push_error(cc::async_error::make_error(cc::any_error(cc::string(_degrade_reason))));
            return;
        }

        _is_open = true;
        _last_access_flush_secs = steady_now();
        _last_gc_secs = steady_now();
        reseed_totals();
        publish_totals();
        msg.promise->push_value(cc::unit{});
    }

    void on_message(get_request msg) override
    {
        if (!is_usable())
        {
            bump(&cache_stats::misses);
            msg.promise->push_value(cc::optional<cache_hit>());
            return;
        }

        auto reader = cache_reader(_db);
        auto found = reader.find_entry(msg.key);
        if (found.has_error())
        {
            report(cc::move(found.error()));
            bump(&cache_stats::misses);
            msg.promise->push_value(cc::optional<cache_hit>());
            return;
        }
        if (!found.value().has_value())
        {
            bump(&cache_stats::misses);
            msg.promise->push_value(cc::optional<cache_hit>());
            return;
        }

        auto const& entry = found.value().value();

        // Expiry is a LOGICAL check, resolved here rather than by waiting for a collection: an expired entry is a miss the instant it expires, whether or not its row still exists.
        if (entry.expires_at > 0 && entry.expires_at <= wall_now())
        {
            // Never deleted on the read path — a get must not take a write lock.
            // Noted for the next pass instead.
            _expired_ids.push_back(entry.entry_id);
            bump(&cache_stats::misses);
            bump(&cache_stats::expired_as_miss);
            msg.promise->push_value(cc::optional<cache_hit>());
            return;
        }

        auto data = reader.read_object(entry);
        if (data.has_error())
        {
            report(cc::move(data.error()));
            bump(&cache_stats::misses);
            msg.promise->push_value(cc::optional<cache_hit>());
            return;
        }

        if (_timing.verify_on_read && !(content_hash::create(data.value()) == entry.hash))
        {
            report(cc::any_error(cc::format("object {} does not match its content hash", entry.object_id)));
            _expired_ids.push_back(entry.entry_id);
            bump(&cache_stats::misses);
            msg.promise->push_value(cc::optional<cache_hit>());
            return;
        }

        note_access(entry.entry_id);
        bump(&cache_stats::hits);
        msg.promise->push_value(cc::optional<cache_hit>(
            cache_hit{.data = data.value(), .hash = entry.hash, .metadata = cc::move(found.value().value().metadata)}));
    }

    void on_message(put_request msg) override
    {
        auto const size = isize(msg.data.size());

        if (_limits.max_object_bytes > 0 && size > _limits.max_object_bytes)
        {
            // Checked before any transaction opens, so an oversized blob never takes a write lock.
            msg.promise->push_value({.status = put_status::rejected_too_large});
            return;
        }

        if (!is_usable())
        {
            bump(&cache_stats::put_failures);
            msg.promise->push_value({.status = put_status::unavailable});
            return;
        }

        auto const now = wall_now();
        auto const hash = content_hash::create(msg.data);
        auto const row = put_row{.key = cc::move(msg.key),
                                 .hash = hash,
                                 .size = size,
                                 .created_at = now,
                                 .expires_at = msg.options.ttl_secs > 0 ? now + msg.options.ttl_secs : 0.0,
                                 .compute_secs = msg.options.compute_time_secs,
                                 .metadata = cc::move(msg.options.metadata)};

        auto writer = cache_writer(_db);
        auto status = writer.insert(row, msg.data);
        if (status.has_error())
        {
            report(cc::move(status.error()));
            bump(&cache_stats::put_failures);
            msg.promise->push_value({.status = put_status::unavailable, .hash = hash});
            return;
        }

        switch (status.value())
        {
        case put_status::stored:
            bump(&cache_stats::puts_stored);
            // Only a real store grows the file, so only it moves the counter that decides whether a pass is due.
            _stored_bytes += size;
            _entry_count += 1;
            break;
        case put_status::deduplicated:
            bump(&cache_stats::puts_deduplicated);
            _entry_count += 1;
            break;
        case put_status::already_present:
            bump(&cache_stats::puts_lost_race);
            break;
        default:
            break;
        }

        if (is_over_limit())
            _gc_requested = true;

        publish_totals();
        msg.promise->push_value({.status = status.value(), .hash = hash});
    }

    void on_message(invalidate_request msg) override
    {
        if (!is_usable())
        {
            msg.promise->push_value(false);
            return;
        }

        auto writer = cache_writer(_db);
        auto removed = writer.remove_entry(msg.key);
        if (removed.has_error())
        {
            report(cc::move(removed.error()));
            msg.promise->push_value(false);
            return;
        }

        _entry_count -= removed.value() ? 1 : 0;
        publish_totals();
        msg.promise->push_value(removed.value());
    }

    void on_message(clear_request msg) override
    {
        if (!is_usable())
        {
            msg.promise->push_value(i64(0));
            return;
        }

        auto writer = cache_writer(_db);
        auto removed = writer.remove_namespace(msg.space);
        if (removed.has_error())
        {
            report(cc::move(removed.error()));
            msg.promise->push_value(i64(0));
            return;
        }

        _entry_count -= removed.value();
        publish_totals();
        msg.promise->push_value(removed.value());
    }

    void on_message(limits_request msg) override
    {
        _limits = normalize(msg.limits);
        if (is_over_limit())
            _gc_requested = true;
    }

    void on_message(gc_request msg) override
    {
        if (!is_usable())
        {
            msg.promise->push_value({});
            return;
        }

        // A pass to completion rather than a slice, so a test has an oracle rather than a schedule.
        begin_pass();
        auto total = gc_result();
        for (auto slice = i64(0); slice < _collector_budget.max_slices_per_pass; ++slice)
        {
            auto const step = run_slice();
            accumulate(total, step);
            if (!_gc_requested)
            {
                msg.promise->push_value(total);
                return;
            }
        }

        total.is_incomplete = true;
        msg.promise->push_value(total);
    }

    void on_message(flush_request msg) override
    {
        flush_access_notes();
        msg.promise->push_value(cc::unit{});
    }

    void on_message(close_request) override
    {
        flush_access_notes();
        _db = {};
        _is_open = false;
    }

    // ---- maintenance -----------------------------------------------------------------------------

    /// Called each loop after the inbox is drained, even when it was empty.
    ///
    /// Returning true means "call me again immediately", which is what makes a collection run in SLICES: the loop
    /// re-drains the inbox between two of them, so a get queued behind a large pass waits for one slice rather than for the whole pass.
    bool on_process() override
    {
        if (!is_usable())
            return false;

        auto const now = steady_now();

        if (_pending_access.size() >= _timing.access_flush_threshold
            || (!_pending_access.empty() && now - _last_access_flush_secs >= _timing.access_flush_interval_secs))
            flush_access_notes();

        if (!_gc_requested && now - _last_gc_secs >= _timing.gc_interval_secs)
        {
            _gc_requested = true;
            _pass_started = false;
        }

        if (!_gc_requested)
            return false;

        if (!_pass_started)
            begin_pass();

        (void)run_slice();
        return _gc_requested;
    }

private:
    // ---- opening ---------------------------------------------------------------------------------

    cc::result<cc::unit> open_database(cc::string_view path)
    {
        if (!sql::is_available())
            return cc::error(cc::any_error(cc::string("no SQLite backend was compiled in")));

        auto opened = sql::database::open(path);
        if (opened.has_error())
            return cc::error(cc::any_error(cc::string(opened.error().message)));
        _db = cc::move(opened.value());

        auto outcome = ensure_schema(_db);
        if (outcome.has_error())
        {
            // The header could not even be read: not a database, or damaged past DROP TABLE.
            // Unlinking is the only recovery, and the WAL and shm siblings must go with it — a stale WAL beside a fresh file corrupts it, and cc::remove_file knows nothing about siblings.
            _db = {};
            cc::remove_file(path);
            cc::remove_file(cc::format("{}-wal", path));
            cc::remove_file(cc::format("{}-shm", path));

            auto retried = sql::database::open(path);
            if (retried.has_error())
                return cc::error(cc::any_error(cc::string(retried.error().message)));
            _db = cc::move(retried.value());

            // One retry only, never a loop: a second failure means the environment is the problem, not the file.
            auto second = ensure_schema(_db);
            if (second.has_error())
            {
                _db = {};
                return cc::error(cc::move(second.error()));
            }
        }
        return cc::unit{};
    }

    cache_limits normalize(cache_limits limits) const
    {
        if (limits.target_total_bytes <= 0 || limits.target_total_bytes > limits.max_total_bytes)
            limits.target_total_bytes = i64(double(limits.max_total_bytes) * 0.9);
        return limits;
    }

    // ---- degradation -----------------------------------------------------------------------------

    /// One flag, one code path, one message.
    /// An absent backend, a missing directory, a read-only file, a full disk and mid-run corruption all land here, and every later message then answers as an empty cache.
    void degrade(cc::any_error error)
    {
        _db = {};
        _is_open = false;
        if (_is_degraded)
            return; // reported once, not per message

        _is_degraded = true;
        _degrade_reason = error.to_string();
        if (_on_error.is_valid())
            _on_error(_degrade_reason);
        publish_totals();
    }

    void report(cc::any_error error)
    {
        if (_on_error.is_valid())
            _on_error(error.to_string());
    }

    bool is_usable() const { return _is_open && !_is_degraded; }

    // ---- clocks ----------------------------------------------------------------------------------

    double wall_now() const { return _wall.is_valid() ? _wall() : cc::current_time_wall_secs(); }
    double steady_now() const { return _steady.is_valid() ? _steady() : cc::current_time_steady_secs(); }

    // ---- access notes ----------------------------------------------------------------------------

    void note_access(i64 entry_id)
    {
        // Quantized, so a hot entry's row is touched once per epoch instead of once per hit.
        // Last note wins per entry, which is what makes a hundred hits one map slot rather than a hundred rows.
        auto const epoch = double(i64(wall_now() / _timing.access_epoch_secs)) * _timing.access_epoch_secs;
        _pending_access[entry_id] = epoch;
    }

    void flush_access_notes()
    {
        if (_pending_access.empty())
            return;

        if (is_usable())
        {
            auto writer = cache_writer(_db);
            auto written = writer.flush_access(_pending_access);
            if (written.has_error())
                report(cc::move(written.error()));
            else
                add(&cache_stats::access_rows_written, written.value());
        }

        // Cleared even on failure.
        // Recency is an approximation, losing a batch costs a slightly worse eviction decision, and a buffer that grew on every failure would be the actual bug.
        _pending_access.clear();
        _last_access_flush_secs = steady_now();
    }

    // ---- collection ------------------------------------------------------------------------------

    bool is_over_limit() const
    {
        if (_limits.max_total_bytes > 0 && _stored_bytes > _limits.max_total_bytes)
            return true;
        return _limits.max_entries > 0 && _entry_count > _limits.max_entries;
    }

    bool is_over_target() const { return bytes_over_target() > 0 || entries_over_limit() > 0; }

    i64 bytes_over_target() const
    {
        if (_limits.max_total_bytes <= 0)
            return 0;
        return cc::max(_stored_bytes - _limits.target_total_bytes, i64(0));
    }

    i64 entries_over_limit() const
    {
        if (_limits.max_entries <= 0)
            return 0;
        return cc::max(_entry_count - _limits.max_entries, i64(0));
    }

    void begin_pass()
    {
        // Recency first, so the scores this pass reads are fresh rather than a flush interval stale.
        flush_access_notes();
        reseed_totals();
        _gc_requested = true;
        _pass_started = true;
        _phase = gc_phase::expiry;
    }

    /// One slice: bounded work, one phase at a time.
    /// Clears _gc_requested once the pass has nothing left, which is what both drivers use as their termination test.
    gc_result run_slice()
    {
        auto out = gc_result();
        auto collector = cache_collector(_db, _collector_budget);

        if (_phase == gc_phase::expiry)
        {
            if (!_expired_ids.empty())
            {
                auto n = collector.remove_known_expired(_expired_ids);
                if (n.has_error())
                    return finish_pass_on_error(cc::move(n.error()));
                out.entries_expired += n.value();
                _entry_count -= n.value();
                _expired_ids.clear();
                return out;
            }

            auto n = collector.remove_expired_batch(wall_now());
            if (n.has_error())
                return finish_pass_on_error(cc::move(n.error()));
            out.entries_expired += n.value();
            _entry_count -= n.value();
            if (n.value() > 0)
                return out; // more may be waiting; stay in this phase

            _phase = gc_phase::eviction;
        }

        if (_phase == gc_phase::eviction)
        {
            if (is_over_target() && _entry_count > 0)
            {
                auto n = collector.evict_batch({.now = wall_now(),
                                                .default_compute_secs = _timing.default_compute_secs,
                                                .half_life_secs = _timing.half_life_secs},
                                               bytes_over_target(), entries_over_limit());
                if (n.has_error())
                    return finish_pass_on_error(cc::move(n.error()));
                out.entries_evicted += n.value();
                _entry_count -= n.value();
                add(&cache_stats::entries_evicted, n.value());
            }

            // Always on to reclamation, even after a batch that evicted plenty.
            //
            // _stored_bytes falls only when an OBJECT goes, so staying here for another batch would re-decide
            // against the same stale total and evict a second time for space the first batch already scheduled to
            // free — which is how the most valuable entry in a small cache gets thrown away.
            // Reclamation comes back here if it freed anything and the cache is still over.
            _phase = gc_phase::reclamation;
        }

        auto reclaimed = collector.reclaim_orphan_batch();
        if (reclaimed.has_error())
            return finish_pass_on_error(cc::move(reclaimed.error()));

        out.objects_reclaimed += reclaimed.value().objects_reclaimed;
        out.bytes_reclaimed += reclaimed.value().bytes_reclaimed;
        _stored_bytes -= reclaimed.value().bytes_reclaimed;
        add(&cache_stats::bytes_reclaimed, reclaimed.value().bytes_reclaimed);

        if (reclaimed.value().objects_reclaimed > 0)
        {
            // Freed bytes may have put us back under the target, so re-enter eviction rather than assuming.
            _phase = gc_phase::eviction;
            return out;
        }

        collector.vacuum_slice();
        end_pass();
        return out;
    }

    gc_result finish_pass_on_error(cc::any_error error)
    {
        report(cc::move(error));
        end_pass();
        return {};
    }

    void end_pass()
    {
        _gc_requested = false;
        _pass_started = false;
        _last_gc_secs = steady_now();
        reseed_totals();
        publish_totals();
    }

    /// Re-read rather than trusted: another process's puts and evictions are invisible to our incremental counter,
    /// and one aggregate over a small table once per pass costs nothing next to being wrong about the ceiling.
    void reseed_totals()
    {
        if (!is_usable())
            return;
        auto reader = cache_reader(_db);
        auto totals = reader.read_totals();
        if (totals.has_error())
        {
            report(cc::move(totals.error()));
            return;
        }
        _stored_bytes = totals.value().stored_bytes;
        _entry_count = totals.value().entry_count;
        _file_bytes = totals.value().file_bytes;
    }

    // ---- counters --------------------------------------------------------------------------------

    void bump(i64 cache_stats::* field) { add(field, 1); }

    void add(i64 cache_stats::* field, i64 n)
    {
        if (auto core = _core.lock())
            core->stats.lock([&](cache_stats& s) { s.*field += n; });
    }

    void publish_totals()
    {
        if (auto core = _core.lock())
            core->stats.lock(
                [&](cache_stats& s)
                {
                    s.stored_bytes = _stored_bytes;
                    s.entry_count = _entry_count;
                    s.file_bytes = _file_bytes;
                    s.is_backed_by_storage = is_usable();
                });
    }

    static void accumulate(gc_result& total, gc_result const& step)
    {
        total.entries_expired += step.entries_expired;
        total.entries_evicted += step.entries_evicted;
        total.objects_reclaimed += step.objects_reclaimed;
        total.bytes_reclaimed += step.bytes_reclaimed;
    }

    // ---- state -----------------------------------------------------------------------------------

    enum class gc_phase : u8
    {
        expiry,
        eviction,
        reclamation
    };

    sql::database _db;
    bool _is_open = false;
    bool _is_degraded = false;
    cc::string _degrade_reason;

    cache_limits _limits;
    timing_policy _timing;
    gc_budget _collector_budget;

    cc::unique_function<double()> _wall;
    cc::unique_function<double()> _steady;
    cc::unique_function<void(cc::string_view)> _on_error;

    /// entry id -> the quantized wall-clock time to write.
    cc::map<i64, double> _pending_access;
    double _last_access_flush_secs = 0;

    /// Entries a get found expired.
    /// Never deleted on the read path, which must not take a write lock.
    cc::vector<i64> _expired_ids;

    i64 _stored_bytes = 0;
    i64 _entry_count = 0;
    i64 _file_bytes = 0;
    double _last_gc_secs = 0;
    bool _gc_requested = false;
    bool _pass_started = false;
    gc_phase _phase = gc_phase::expiry;

    std::weak_ptr<cache_core> _core;
};
} // namespace

cc::unique_ptr<cache_actor> make_cache_actor(bool unthreaded)
{
    auto actor = cc::make_threaded_actor<cache_actor_impl>();
    actor->start(unthreaded ? cc::threaded_actor_mode::unthreaded : cc::threaded_actor_mode::threaded_if_possible);
    return actor;
}
} // namespace bcache::impl
