#include <clean-core/common/assertf.hh>
#include <shaped-graphics/raytracing/partitioned_tlas.hh>

namespace sg
{
partitioned_tlas::partitioned_tlas(partitioned_tlas_description const& description,
                                   isize size_in_bytes,
                                   isize build_scratch_size_in_bytes,
                                   isize update_scratch_size_in_bytes)
  : tlas(size_in_bytes,
         build_scratch_size_in_bytes,
         update_scratch_size_in_bytes,
         description.flags,
         description.instance_capacity,
         {}),
    _description(description)
{
    _slots.lock([&](cc::vector<impl::ptlas_slot>& slots) { slots.resize_to_defaulted(description.instance_capacity); });
}

cc::vector<blas_handle> impl::apply_to_slots(partitioned_tlas const& ptlas, ptlas_operations const& ops)
{
    auto const& d = ptlas.description();
    auto const in_partition_range = [&](int partition)
    { return partition == global_partition || (partition >= 0 && partition < d.partition_count); };

    return ptlas._slots.lock(
        [&](cc::vector<ptlas_slot>& slots)
        {
            // Each slot once per update, since the operations apply in no order among themselves.
            auto named = cc::vector<bool>::create_filled(slots.size(), false);
            auto const name_slot = [&](int index, char const* what)
            {
                CC_ASSERTF(index >= 0 && index < slots.size(), "update: {} names slot {}, outside capacity {}", what,
                           index, slots.size());
                CC_ASSERTF(!named[index], "update: slot {} is named twice in one update", index);
                named[index] = true;
            };

            auto released = cc::vector<blas_handle>();
            auto const replace = [&](ptlas_slot& slot, blas_handle blas)
            {
                if (slot.blas != nullptr && slot.blas != blas)
                    released.push_back(cc::move(slot.blas));
                slot.blas = cc::move(blas);
            };

            for (auto const& w : ops.writes)
            {
                name_slot(w.index, "a write");
                CC_ASSERTF(in_partition_range(w.partition), "update: a write names partition {}, of {}", w.partition,
                           d.partition_count);
                CC_ASSERT(w.instance.blas != nullptr && !w.instance.blas->is_expired(), "update: a write's blas is "
                                                                                        "null or expired");
                CC_ASSERT(!w.instance.blas->is_stale(), "update: a write's cluster BLAS is stale: its pool was rebuilt "
                                                        "since");
                CC_ASSERT(!w.instance.transform_at_end.has_value(), "update: a partitioned tlas has no motion, so a "
                                                                    "write's instance must not move");
                CC_ASSERT(w.instance.instance_id < (1u << 24), "update: instance_id must fit in 24 bits");
                CC_ASSERT(w.instance.hit_group_offset < (1u << 24), "update: hit_group_offset must fit in 24 bits");
                auto& slot = slots[w.index];
                replace(slot, w.instance.blas);
                slot.hit_group_offset = w.instance.hit_group_offset;
                slot.mask = w.instance.mask;
            }
            for (auto const& u : ops.updates)
            {
                name_slot(u.index, "an update");
                CC_ASSERT(u.blas != nullptr && !u.blas->is_expired(), "update: an update's blas is null or expired");
                CC_ASSERT(!u.blas->is_stale(), "update: an update's cluster BLAS is stale: its pool was rebuilt since");
                CC_ASSERT(u.hit_group_offset < (1u << 24), "update: hit_group_offset must fit in 24 bits");
                auto& slot = slots[u.index];
                CC_ASSERTF(slot.blas != nullptr, "update: slot {} is empty, so only a write can fill it", u.index);
                replace(slot, u.blas);
                slot.hit_group_offset = u.hit_group_offset;
            }
            for (auto const index : ops.clears)
            {
                name_slot(index, "a clear");
                auto& slot = slots[index];
                replace(slot, nullptr);
                slot = {};
            }

            auto translated = cc::vector<bool>::create_filled(d.partition_count + 1, false);
            for (auto const& t : ops.translations)
            {
                CC_ASSERT(d.allows_partition_translation, "update: a translation needs "
                                                          "partitioned_tlas_description::allows_partition_translation");
                CC_ASSERTF(in_partition_range(t.partition), "update: a translation names partition {}, of {}",
                           t.partition, d.partition_count);
                auto const at = t.partition == global_partition ? d.partition_count : t.partition;
                CC_ASSERTF(!translated[at], "update: partition {} is translated twice in one update", t.partition);
                translated[at] = true;
            }
            return released;
        });
}

cc::vector<impl::tlas_instance_record> impl::slot_records(partitioned_tlas const& ptlas)
{
    return ptlas._slots.lock(
        [](cc::vector<ptlas_slot> const& slots)
        {
            auto records = cc::vector<tlas_instance_record>();
            records.reserve(slots.size());
            for (auto const& s : slots)
                records.push_back({.blas = s.blas, .hit_group_offset = s.hit_group_offset, .mask = s.mask});
            return records;
        });
}

bool impl::mark_updated(partitioned_tlas const& ptlas)
{
    return !ptlas._has_been_updated.exchange(true, std::memory_order_acq_rel);
}

cc::vector<blas_table_handle> impl::hold_tables(partitioned_tlas const& ptlas, cc::span<blas_table_handle const> tables)
{
    return ptlas._tables.lock(
        [&](cc::vector<blas_table_handle>& held)
        {
            auto released = cc::move(held);
            held = cc::vector<blas_table_handle>::create_copy_of(tables);
            return released;
        });
}

bool impl::mark_gpu_written(partitioned_tlas const& ptlas)
{
    clear_instance_records(ptlas);
    return !ptlas._is_gpu_written.exchange(true, std::memory_order_acq_rel);
}
} // namespace sg
