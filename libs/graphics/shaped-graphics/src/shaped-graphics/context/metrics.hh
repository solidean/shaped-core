#pragma once

#include <clean-core/common/flags.hh>
#include <clean-core/common/time.hh>
#include <clean-core/error/result.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/atomic.hh>
#include <shaped-graphics/context/adapter_info.hh>
#include <shaped-graphics/context/gpu_metrics.hh>
#include <shaped-graphics/fwd.hh>

/// What a context reports about the device it runs on and about its own activity.
/// See libs/graphics/shaped-graphics/docs/concepts/metrics.md.

// Every stat, once: its enumerator, its unit, and what it counts; it records as `sg.` and its enumerator.
// The enum, the name table and the cc::rec bridge are all generated from this list, so they cannot drift apart.
// The bridge needs the name as a literal at each site, which is why this is a macro rather than a table.
// A comment cannot sit on one of its lines, since line splicing runs first, so what a stat counts is its last argument.
#define SG_IMPL_STATS(X)                                                                                    \
    X(draws, count, "every draw a submitted list recorded, indexed or not")                                 \
    X(dispatches, count, "every compute dispatch a submitted list recorded")                                \
    X(ray_dispatches, count, "every ray dispatch a submitted list recorded")                                \
    X(command_lists_submitted, count, "every list submitted, the ones sg submits on its own included")      \
    X(epochs_advanced, count, "every advance_epoch")                                                        \
    X(buffer_barriers, count, "one per buffer barrier record the backend emitted")                          \
    X(texture_barriers, count, "one per texture barrier record the backend emitted, per subresource range") \
    X(texture_transitions, count, "the texture barriers that also changed the layout")                      \
    X(global_barriers, count, "barriers naming stages and no resource")                                     \
    X(barrier_calls, count, "one per API call that submitted a batch of barriers")                          \
    X(render_pass_splits, count, "a rendering scope ended and reopened mid-scope, whatever forced it")      \
    X(bytes_uploaded_inline, bytes, "cmd.upload bytes, counted on the list")                                \
    X(bytes_uploaded_async, bytes, "async upload bytes, at enqueue")                                        \
    X(bytes_uploaded_stream, bytes, "stream upload bytes, at enqueue, or per chunk for a buffer source")    \
    X(bytes_downloaded_inline, bytes, "cmd.download bytes, counted on the list")                            \
    X(bytes_downloaded_async, bytes, "async download bytes, at enqueue")                                    \
    X(bytes_downloaded_stream, bytes, "stream download bytes, at enqueue")                                  \
    X(bytes_inline_overflow, bytes, "inline bytes that missed their ring and got a one-off allocation")     \
    X(async_layout_fixups, count, "async transfers that had to submit a texture transition of their own")   \
    X(pipelines_created, count, "pipelines built by ctx.uncached, which a cache hit never reaches")         \
    X(binding_groups_created, count, "binding groups created, and every staging-group snapshot minted")     \
    X(buffers_created, count, "buffers created, persistent or transient")                                   \
    X(textures_created, count, "textures created, persistent or transient")                                 \
    X(gpu_wait_nanoseconds, nanoseconds, "time blocked on the GPU in the context's waits, which vary per backend")

/// One monotone total a context counts: it only ever grows, so two readings subtract into "what happened between".
///
/// **Barriers are counted as the backend emits them**: one per buffer or texture barrier record, one per call that
/// submits a batch, and a texture barrier that changes the layout is also a `texture_transitions`.
/// A barrier naming stages and no resource — metal's, or one a missing array declaration falls back to — is a `global_barriers`.
///
/// **Transfer bytes count when the transfer is enqueued**, not when it lands, so a reading is deterministic.
/// A stream from a buffer source is the exception: its size is unknown up front, so it counts each chunk as the copy
/// thread takes it.
enum class sg::stat : sg::u8
{
#define SG_IMPL_STAT_ENUMERATOR(id_, u_, what_) id_,
    SG_IMPL_STATS(SG_IMPL_STAT_ENUMERATOR)
#undef SG_IMPL_STAT_ENUMERATOR
};

CC_FLAG_ENUM_INDEXED(sg, stat, u64);

namespace sg
{
/// Which stats a backend counts; the others read zero without meaning it.
using stat_set = cc::flags<stat>;

/// How many stats there are.
inline constexpr int stat_count = 0
#define SG_IMPL_STAT_COUNT(id_, u_, what_) +1
    SG_IMPL_STATS(SG_IMPL_STAT_COUNT)
#undef SG_IMPL_STAT_COUNT
    ;

/// Every stat, which is what a backend counts unless it names the ones it cannot see.
inline constexpr stat_set all_stats = stat_set::create_from_bits((u64(1) << stat_count) - 1);

/// Every barrier stat, which a backend that cannot see barriers leaves out.
inline constexpr stat_set barrier_stats = stat::buffer_barriers | stat::texture_barriers | stat::texture_transitions
                                        | stat::global_barriers | stat::barrier_calls;
} // namespace sg

/// What a stat is called when it is recorded, what it counts in, and what it counts.
struct sg::stat_info
{
    cc::string_view name;
    cc::rec::unit const* unit = nullptr;
    cc::string_view description;
};

namespace sg
{
/// `s`'s recorded name, unit and description.
[[nodiscard]] stat_info info_of(stat s);
} // namespace sg

/// A reading of every stat at one moment, or the difference between two.
///
/// A plain value: take one, do the work, take another, subtract.
/// A stat the backend does not count reads zero, and `is_counted` says so — a barrier count on webgpu is zero because
/// nobody can see the barriers, not because there were none.
class sg::stats
{
public:
    [[nodiscard]] i64 operator[](stat s) const { return _values[int(s)]; }

    /// Whether the backend that produced this reading counts `s` at all.
    [[nodiscard]] bool is_counted(stat s) const { return _counted.has(s); }

    [[nodiscard]] stat_set counted() const { return _counted; }

    /// What happened between `before` and `after`, per stat.
    /// Both must come from the same context.
    [[nodiscard]] friend sg::stats operator-(sg::stats const& after, sg::stats const& before)
    {
        auto d = after;
        for (auto i = 0; i < stat_count; ++i)
            d._values[i] -= before._values[i];
        return d;
    }

private:
    friend class impl::stat_totals;

    i64 _values[stat_count] = {};
    stat_set _counted;
};

/// Counts a command list records while it is open, on its one recording thread.
/// Folded into the context's totals when the list is submitted; a dropped list counts nothing.
struct sg::impl::stat_counts
{
    i64 values[stat_count] = {};

    void add(stat s, i64 n = 1) { values[int(s)] += n; }
};

/// A context's running totals.
///
/// Written with relaxed atomic adds — a list's counts all at once when it submits, a context-level event at its call —
/// so a reading taken while another thread submits may include part of that list.
/// Each value is right on its own; a reading taken right after `advance_epoch`, on the thread that advanced, is a complete epoch.
class sg::impl::stat_totals
{
public:
    void add(stat s, i64 n = 1) { _values[int(s)].fetch_add(n, cc::memory_order_relaxed); }

    void fold(stat_counts const& counts);

    /// Set once by a backend that cannot see every stat, before the context is handed out.
    void set_counted(stat_set counted) { _counted = counted; }

    [[nodiscard]] sg::stats snapshot() const;

private:
    cc::atomic<i64> _values[stat_count] = {};
    stat_set _counted = all_stats;
};

/// Adds the time from construction to destruction to `gpu_wait_nanoseconds`.
/// Placed around a call that blocks the CPU on the GPU — a fence wait, a semaphore wait — and nothing else.
class sg::impl::gpu_wait_scope
{
public:
    explicit gpu_wait_scope(stat_totals& totals) : _totals(totals), _start(cc::current_time_steady_secs()) {}
    ~gpu_wait_scope() { _totals.add(stat::gpu_wait_nanoseconds, i64((cc::current_time_steady_secs() - _start) * 1e9)); }

    gpu_wait_scope(gpu_wait_scope const&) = delete;
    gpu_wait_scope& operator=(gpu_wait_scope const&) = delete;

private:
    stat_totals& _totals;
    double _start;
};

/// What a context can say about the device it runs on, and about its own activity: `ctx.metrics.stats()`.
///
/// Free-threaded, all of it.
class sg::context_metrics_scope
{
public:
    /// Which GPU this context is running on, fixed at creation.
    /// Fields a backend cannot report are left at their defaults, so a caller reads "unknown" and never a wrong answer.
    [[nodiscard]] adapter_info const& adapter() const;

    /// What this process may use of the GPU's memory right now, and what it is using.
    ///
    /// dx12 answers through DXGI, and vulkan where VK_EXT_memory_budget is present; webgpu and metal refuse.
    /// The card's own size is `adapter().dedicated_video_memory_bytes`, and the two are different scales — see there.
    /// Refuses rather than fabricating a zero where the backend cannot answer.
    [[nodiscard]] cc::result<gpu_memory_usage> query_gpu_memory() const;

    /// Monotone busy time per GPU engine class, for sg::gpu_load_sampler to difference.
    ///
    /// **Neither D3D12 nor Vulkan exposes utilization**, so this comes from the OS instead: the GPU Engine performance
    /// counters on Windows, `raw:/sys/class/drm/*/device/gpu_busy_percent` on Linux where the driver provides one,
    /// IOKit on macOS.
    /// Only the Windows path exists today; everywhere else this refuses rather than guessing.
    ///
    /// A caller almost always wants sg::gpu_load_sampler rather than this — the counters are published because a rate
    /// has thrown the seconds away and somebody always wants them back.
    [[nodiscard]] cc::result<gpu_counters> read_gpu_counters() const;

    /// Every stat's total so far.
    /// Current as of each call that caused it: a list's counts once its submit returns, a transfer's bytes once its enqueue does.
    [[nodiscard]] sg::stats stats() const;

    /// Whether a rendering scope that a backend has to close and reopen mid-scope says so, as a warning.
    /// **On by default**: a split stores and reloads every target, which on a tiler is the most expensive thing a frame does by accident.
    /// A backend whose native pass cannot hold an operation splits around it — vulkan and a copy recorded inside the scope is the plain case.
    /// dx12 has no such pass and never splits.
    /// **Once per context and cause**, so a split every frame reports on the first and the `render_pass_splits` stat carries the rest.
    /// A program that splits knowingly turns it off, at any time; the stat counts them either way.
    /// See libs/graphics/shaped-graphics/docs/concepts/barriers.md.
    [[nodiscard]] bool render_pass_split_warnings() const;
    void set_render_pass_split_warnings(bool enabled);

    // Pinned to its owning context: neither copyable nor movable.
    context_metrics_scope(context_metrics_scope const&) = delete;
    context_metrics_scope(context_metrics_scope&&) = delete;
    context_metrics_scope& operator=(context_metrics_scope const&) = delete;
    context_metrics_scope& operator=(context_metrics_scope&&) = delete;

private:
    friend class context;
    explicit context_metrics_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
