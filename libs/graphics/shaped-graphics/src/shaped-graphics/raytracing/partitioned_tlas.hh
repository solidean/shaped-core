#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>

#include <atomic>

/// A partitioned top-level acceleration structure: a `tlas` of fixed capacity, updated in place by recorded operations.
/// Its instances live in slots, each in one partition or in the global one, and an update rebuilds only the partitions it touches.
/// Created empty through `ctx.persistent.create_partitioned_tlas`, and filled by `cmd.raytracing.update`.
/// It binds and traces as any tlas does, through `as_view()`.
/// Requires `sg::feature::partitioned_tlas`.
/// See libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md.

namespace sg
{
/// The partition an instance moves every frame in: each instance there is treated as a partition of its own.
inline constexpr int global_partition = -1;
} // namespace sg

/// What a partitioned_tlas is created for, all of it fixed for its life.
/// A different capacity is a different partitioned_tlas.
struct sg::partitioned_tlas_description
{
    /// Instance slots, indexed 0 .. instance_capacity - 1, every one empty until written.
    /// Must be > 0.
    int instance_capacity = 0;

    /// Partitions besides the global one, indexed 0 .. partition_count - 1.
    int partition_count = 0;

    /// The most instances one partition holds at a time, which must be > 0 where `partition_count` is.
    int max_instances_per_partition = 0;

    /// The most instances the global partition holds at a time.
    int max_global_instances = 0;

    /// Whether `ptlas_translation`s may move whole partitions, the global one included.
    bool allows_partition_translation = false;

    /// `fast_trace` or `fast_build`, and nothing else.
    accel_build_flags flags = accel_build_flag::fast_trace;
};

/// Writes slot `index` whole: its instance and the partition it is in.
/// The instance's `blas` must be non-null and built, and the instance must not move (`transform_at_end` unset).
struct sg::ptlas_write
{
    int index = 0;
    /// A partition below `partition_count`, or `global_partition`.
    int partition = global_partition;
    tlas_instance instance = {};
};

/// Replaces the blas and the hit-group offset of slot `index`, which a write must have filled.
/// The cheapest operation, since no partition is rebuilt for it.
struct sg::ptlas_update
{
    int index = 0;
    blas_handle blas = nullptr;
    /// 24-bit, as `tlas_instance::hit_group_offset`.
    u32 hit_group_offset = 0;
};

/// Moves partition `partition` by `translation`, replacing its previous translation rather than adding to it.
/// Requires `partitioned_tlas_description::allows_partition_translation`.
struct sg::ptlas_translation
{
    /// A partition below `partition_count`, or `global_partition`.
    int partition = global_partition;
    float translation[3] = {0, 0, 0};
};

/// One update's operations, which apply together and in no order among themselves.
/// So a slot is named at most once across `writes`, `updates` and `clears`, and a partition at most once in `translations`.
struct sg::ptlas_operations
{
    cc::span<ptlas_write const> writes = {};
    cc::span<ptlas_update const> updates = {};
    /// Slots to empty, which no ray then meets.
    cc::span<int const> clears = {};
    cc::span<ptlas_translation const> translations = {};
};

namespace sg::impl
{
/// What a partitioned_tlas knows of one slot on the CPU, as the operations recorded so far left it.
struct ptlas_slot
{
    blas_handle blas;
    u32 hit_group_offset = 0;
    u8 mask = 0xFF;
};

/// Applies `ops` to the slots of `ptlas`, as an update records them, and hands back the blases no slot holds any more.
/// The caller keeps those alive until the recorded update has run.
[[nodiscard]] cc::vector<blas_handle> apply_to_slots(partitioned_tlas const& ptlas, ptlas_operations const& ops);

/// Every slot's instance record, for `context::hit_record_checks`, as the updates recorded so far left it.
[[nodiscard]] cc::vector<tlas_instance_record> slot_records(partitioned_tlas const& ptlas);

/// Marks `ptlas` updated, and answers whether this is its first update, which a backend builds from no source.
bool mark_updated(partitioned_tlas const& ptlas);

/// Makes `tables` the ones `ptlas` holds for its GPU-written records, and hands back those it held before.
/// The caller keeps those alive until the recorded update has run.
[[nodiscard]] cc::vector<blas_table_handle> hold_tables(partitioned_tlas const& ptlas,
                                                        cc::span<blas_table_handle const> tables);

/// Marks `ptlas` written from the GPU, whose instances no check can follow any more, and drops its instance records.
/// Answers whether this is the first such update.
bool mark_gpu_written(partitioned_tlas const& ptlas);
} // namespace sg::impl

/// Abstract: a backend subclasses it and owns the native structure, as with `tlas`.
/// The slots are the ownership edge: a partitioned_tlas holds every blas a slot names, and drops one only once no
/// recorded update can still read it.
class sg::partitioned_tlas : public sg::tlas
{
public:
    [[nodiscard]] partitioned_tlas_description const& description() const { return _description; }

    /// Whether an update was recorded into it, which a trace needs: it is built by its first update.
    [[nodiscard]] bool has_been_updated() const { return _has_been_updated.load(std::memory_order_acquire); }

    /// Whether an update was recorded from GPU-written records, whose instances no check follows from then on.
    [[nodiscard]] bool is_gpu_written() const { return _is_gpu_written.load(std::memory_order_acquire); }

protected:
    partitioned_tlas(partitioned_tlas_description const& description,
                     isize size_in_bytes,
                     isize build_scratch_size_in_bytes,
                     isize update_scratch_size_in_bytes);

private:
    friend cc::vector<blas_handle> impl::apply_to_slots(partitioned_tlas const& ptlas, ptlas_operations const& ops);
    friend cc::vector<impl::tlas_instance_record> impl::slot_records(partitioned_tlas const& ptlas);
    friend bool impl::mark_updated(partitioned_tlas const& ptlas);
    friend cc::vector<blas_table_handle> impl::hold_tables(partitioned_tlas const& ptlas,
                                                           cc::span<blas_table_handle const> tables);
    friend bool impl::mark_gpu_written(partitioned_tlas const& ptlas);

    partitioned_tlas_description _description;
    mutable std::atomic<bool> _has_been_updated = {false};
    mutable std::atomic<bool> _is_gpu_written = {false};
    mutable cc::mutex<cc::vector<impl::ptlas_slot>> _slots; // mutable: updates are recorded through a const handle
    mutable cc::mutex<cc::vector<blas_table_handle>> _tables;
};
