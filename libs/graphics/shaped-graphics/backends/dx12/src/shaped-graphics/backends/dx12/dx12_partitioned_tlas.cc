// dx12 partitioned tlas: the preview runtime's ExecuteIndirectRTASOperations behind ctx.persistent.create_partitioned_tlas
// and cmd.raytracing.update.
// Every preview name stays behind SG_HAS_AGILITY_PREVIEW, since the spec still renames fields between previews.
// See libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md.

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh> // offsetof
#include <shaped-graphics/backends/dx12/dx12_acceleration_structure.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/context/impl/device_lifecycle.hh>
#include <shaped-graphics/raytracing/blas_table.hh>
#include <shaped-graphics/raytracing/partitioned_tlas.hh>

#ifndef SG_HAS_AGILITY_PREVIEW
#error "SG_HAS_AGILITY_PREVIEW must be defined by the dx12 backend's CMake"
#endif

namespace sg::backend::dx12
{
u64 dx12_context::blas_address_of(sg::blas const& blas) const
{
    auto const* const dx = dynamic_cast<dx12_blas const*>(&blas);
    CC_ASSERT(dx != nullptr, "blas_address_of: a blas is not a dx12 blas");
    return u64(dx->_dx12_storage->gpu_virtual_address());
}

u64 dx12_context::buffer_address_of(sg::raw_buffer const& buffer) const
{
    auto const* const dx = dynamic_cast<dx12_buffer const*>(&buffer);
    CC_ASSERT(dx != nullptr, "buffer_address_of: a buffer is not a dx12 buffer");
    return u64(dx->gpu_virtual_address());
}

#if SG_HAS_AGILITY_PREVIEW
namespace
{
[[nodiscard]] D3D12_RTAS_PARTITIONED_TLAS_INPUTS_DESC inputs_of(sg::partitioned_tlas_description const& d)
{
    auto flags = D3D12_RTAS_PARTITIONED_TLAS_FLAG_NONE;
    if (d.flags.has(sg::accel_build_flag::fast_trace))
        flags |= D3D12_RTAS_PARTITIONED_TLAS_FLAG_FAST_TRACE;
    if (d.flags.has(sg::accel_build_flag::fast_build))
        flags |= D3D12_RTAS_PARTITIONED_TLAS_FLAG_FAST_BUILD;
    if (d.allows_partition_translation)
        flags |= D3D12_RTAS_PARTITIONED_TLAS_FLAG_ENABLE_PARTITION_TRANSLATION;
    return {.Flags = flags,
            .InstanceCount = UINT(d.instance_capacity),
            .MaxInstancePerPartitionCount = UINT(d.max_instances_per_partition),
            .PartitionCount = UINT(d.partition_count),
            .MaxInstanceInGlobalPartitionCount = UINT(d.max_global_instances)};
}

[[nodiscard]] UINT partition_index_of(int partition)
{
    return partition == sg::global_partition ? UINT(D3D12_RTAS_PARTITIONED_TLAS_PARTITION_INDEX_GLOBAL_PARTITION)
                                             : UINT(partition);
}

[[nodiscard]] UINT instance_flags_of(sg::tlas_instance const& inst)
{
    auto flags = D3D12_RTAS_PARTITIONED_TLAS_INSTANCE_FLAG_NONE;
    switch (inst.cull_mode)
    {
    case sg::instance_cull_mode::back:
        break;
    case sg::instance_cull_mode::front:
        flags |= D3D12_RTAS_PARTITIONED_TLAS_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
        break;
    case sg::instance_cull_mode::none:
        flags |= D3D12_RTAS_PARTITIONED_TLAS_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        break;
    }
    if (inst.opaque_override.has_value())
        flags |= inst.opaque_override.value() ? D3D12_RTAS_PARTITIONED_TLAS_INSTANCE_FLAG_FORCE_OPAQUE
                                              : D3D12_RTAS_PARTITIONED_TLAS_INSTANCE_FLAG_FORCE_NON_OPAQUE;
    return UINT(flags);
}

[[nodiscard]] dx12_blas const& dx12_blas_of(sg::blas_handle const& blas)
{
    auto const* const dx = dynamic_cast<dx12_blas const*>(blas.get());
    CC_ASSERT(dx != nullptr, "update: a blas is not a dx12 blas");
    return *dx;
}

[[nodiscard]] isize aligned(isize at, isize alignment)
{
    return (at + alignment - 1) / alignment * alignment;
}
} // namespace

cc::result<sg::partitioned_tlas_handle> dx12_context::try_create_partitioned_tlas(sg::partitioned_tlas_description const& desc)
{
    if (!_clusters_and_ptlas)
        return cc::error("create_partitioned_tlas: this device reports no ClustersAndPTLASSupported");
    ComPtr<ID3D12DeviceRaytracing2> device;
    if (FAILED(_device.As(&device)) || device == nullptr)
        return cc::error("create_partitioned_tlas: the runtime has no ID3D12DeviceRaytracing2");

    auto const ptlas_inputs = inputs_of(desc);
    auto const inputs = D3D12_RTAS_OPERATION_INPUTS{.Type = D3D12_RTAS_OPERATION_TYPE_PARTITIONED_TLAS,
                                                    .pPartitionedTLASInputsDesc = &ptlas_inputs};
    auto prebuild = D3D12_RTAS_OPERATION_PREBUILD_INFO{};
    {
        sg::impl::raytracing_driver_hold const driver_guard;
        device->GetRTASOperationPrebuildInfo(&inputs, &prebuild);
    }
    if (prebuild.ResultDataMaxSizeInBytes == 0)
        return cc::error("create_partitioned_tlas: the device sized the structure at zero bytes");

    // a committed buffer starts 64 KiB aligned, past the 256 a structure needs
    auto storage_raw = persistent.create_raw_buffer(isize(prebuild.ResultDataMaxSizeInBytes),
                                                    sg::buffer_usage::accel_structure_storage);
    auto storage = std::dynamic_pointer_cast<dx12_buffer const>(storage_raw);
    CC_ASSERT(storage != nullptr, "the structure's storage is not a dx12 buffer");
    return sg::partitioned_tlas_handle(std::make_shared<dx12_partitioned_tlas>(
        desc, cc::move(storage), isize(prebuild.ResultDataMaxSizeInBytes), isize(prebuild.ScratchDataSizeInBytes)));
}

void dx12_command_list::raytracing_update_partitioned_tlas(sg::partitioned_tlas const& ptlas,
                                                           sg::ptlas_operations const& ops,
                                                           bool is_first,
                                                           cc::vector<sg::blas_handle>&& released)
{
    auto const& dx = static_cast<dx12_partitioned_tlas const&>(ptlas);
    ComPtr<ID3D12CommandListRaytracing2> list;
    [[maybe_unused]] HRESULT const list_hr = _list.As(&list);
    CC_ASSERT(SUCCEEDED(list_hr) && list, "update: the command list has no ID3D12CommandListRaytracing2");

    // The arguments as the preview runtime lays them out, and the blases they read.
    auto writes = cc::vector<D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS>();
    auto updates = cc::vector<D3D12_RTAS_PARTITIONED_TLAS_OPERATION_UPDATE_INSTANCE_ARGS>();
    auto translations = cc::vector<D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TRANSLATE_PARTITION_ARGS>();
    auto read_storage = cc::vector<dx12_buffer_handle>();
    for (auto const& w : ops.writes)
    {
        auto const& blas = dx12_blas_of(w.instance.blas);
        auto args = D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS{
            .InstanceID = w.instance.instance_id,
            .InstanceMask = w.instance.mask,
            .InstanceContributionToHitGroupIndex = w.instance.hit_group_offset,
            .InstanceFlags = instance_flags_of(w.instance),
            .AccelerationStructure = blas._dx12_storage->gpu_virtual_address(),
            .InstanceIndex = UINT(w.index),
            .PartitionIndex = partition_index_of(w.partition),
        };
        for (int k = 0; k < 12; ++k) // both row-major 3x4: element k maps straight through
            (&args.Transform[0][0])[k] = w.instance.transform[k];
        writes.push_back(args);
        read_storage.push_back(blas._dx12_storage);
    }
    // a clear writes the slot with no structure, which disables it
    for (auto const index : ops.clears)
        writes.push_back({.InstanceIndex = UINT(index),
                          .PartitionIndex = UINT(D3D12_RTAS_PARTITIONED_TLAS_PARTITION_INDEX_GLOBAL_PARTITION),
                          .AccelerationStructure = 0});
    for (auto const& u : ops.updates)
    {
        auto const& blas = dx12_blas_of(u.blas);
        updates.push_back({.InstanceIndex = UINT(u.index),
                           .InstanceContributionToHitGroupIndex = u.hit_group_offset,
                           .AccelerationStructure = blas._dx12_storage->gpu_virtual_address()});
        read_storage.push_back(blas._dx12_storage);
    }
    for (auto const& t : ops.translations)
        translations.push_back({.PartitionIndex = partition_index_of(t.partition),
                                .PartitionTranslation = {t.translation[0], t.translation[1], t.translation[2]}});

    // One transient buffer: the operations, their count, then each kind's arguments at their own alignment.
    auto const ops_at = isize(0);
    auto const op_count = isize(!writes.empty()) + isize(!updates.empty()) + isize(!translations.empty());
    auto const count_at = ops_at + op_count * isize(sizeof(D3D12_RTAS_PARTITIONED_TLAS_OPERATION));
    auto const writes_at = aligned(count_at + isize(sizeof(UINT)), 16);
    auto const updates_at = aligned(writes_at + writes.size() * isize(sizeof(writes[0])), 16);
    auto const translations_at = aligned(updates_at + updates.size() * isize(sizeof(updates[0])), 16);
    auto const total = translations_at + translations.size() * isize(sizeof(translations[0]));

    auto const input_raw = _ctx.transient.create_raw_buffer(
        total, sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto const input = std::dynamic_pointer_cast<dx12_buffer const>(input_raw);
    CC_ASSERT(input != nullptr, "the update's input buffer is not a dx12 buffer");
    auto const at = [&](isize offset) { return input->gpu_virtual_address() + D3D12_GPU_VIRTUAL_ADDRESS(offset); };

    auto operations = cc::vector<D3D12_RTAS_PARTITIONED_TLAS_OPERATION>();
    if (!writes.empty())
        operations.push_back({.Type = D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TYPE_WRITE_INSTANCE,
                              .ArgCount = UINT(writes.size()),
                              .ArgData = {.StartAddress = at(writes_at), .StrideInBytes = sizeof(writes[0])}});
    if (!updates.empty())
        operations.push_back({.Type = D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TYPE_UPDATE_INSTANCE,
                              .ArgCount = UINT(updates.size()),
                              .ArgData = {.StartAddress = at(updates_at), .StrideInBytes = sizeof(updates[0])}});
    if (!translations.empty())
        operations.push_back({.Type = D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TYPE_TRANSLATE_PARTITION,
                              .ArgCount = UINT(translations.size()),
                              .ArgData = {.StartAddress = at(translations_at), .StrideInBytes = sizeof(translations[0])}});

    auto bytes = cc::vector<byte>::create_filled(total, byte(0));
    auto const put = [&](isize offset, cc::span<byte const> part)
    { cc::memcpy(bytes.data() + offset, part.data(), size_t(part.size())); };
    put(ops_at, cc::as_bytes(cc::span<D3D12_RTAS_PARTITIONED_TLAS_OPERATION const>(operations)));
    auto const count = UINT(operations.size());
    put(count_at, cc::as_bytes(cc::span<UINT const>(&count, 1)));
    put(writes_at, cc::as_bytes(cc::span<D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS const>(writes)));
    put(updates_at, cc::as_bytes(cc::span<D3D12_RTAS_PARTITIONED_TLAS_OPERATION_UPDATE_INSTANCE_ARGS const>(updates)));
    put(translations_at,
        cc::as_bytes(cc::span<D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TRANSLATE_PARTITION_ARGS const>(translations)));
    upload_bytes_to_buffer(input_raw, cc::span<byte const>(bytes), 0);

    auto const scratch_size = dx.build_scratch_size_in_bytes();
    auto const scratch_raw = _ctx.transient.create_raw_buffer(scratch_size > 0 ? scratch_size : isize(1),
                                                              sg::buffer_usage::readwrite_buffer);
    auto const scratch = std::dynamic_pointer_cast<dx12_buffer const>(scratch_raw);
    CC_ASSERT(scratch != nullptr, "the update's scratch is not a dx12 buffer");

    // The structure is read and written in place; its operations and arguments are shader reads, which the preview
    // runtime takes them as rather than as indirect arguments; each blas a write or an update places is read.
    track_buffer_access(dx._dx12_storage, sg::pipeline_stage_flag::accel_build,
                        sg::access_flag::accel_read | sg::access_flag::accel_write);
    track_buffer_access(scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(input, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    for (auto const& s : read_storage)
        track_buffer_access(s, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_read);
    flush_barriers();

    auto const ptlas_inputs = inputs_of(ptlas.description());
    auto const data = D3D12_RTAS_PARTITIONED_TLAS_OPERATION_DATA{
        .SourceAccelerationStructureData = is_first ? 0 : dx._dx12_storage->gpu_virtual_address(),
        .DestAccelerationStructureData = dx._dx12_storage->gpu_virtual_address(),
        .ScratchAccelerationStructureData = scratch->gpu_virtual_address(),
        .IndirectPartitionedTlasOpCount = at(count_at),
        .IndirectPartitionedTlasOps = at(ops_at),
    };
    auto const desc = D3D12_RTAS_OPERATION_DESC{
        .Inputs = {.Type = D3D12_RTAS_OPERATION_TYPE_PARTITIONED_TLAS, .pPartitionedTLASInputsDesc = &ptlas_inputs},
        .pPartitionedTlasOperationData = &data};
    {
        sg::impl::raytracing_driver_hold const driver_guard;
        list->ExecuteIndirectRTASOperations(1, &desc, D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
    }

    // the blases no slot names any more, until this list has run; and the structure itself
    for (auto& blas : released)
        keep_bound(cc::move(blas));
    keep_bound(ptlas.shared_from_this());
}

void dx12_command_list::raytracing_update_partitioned_tlas_indirect(sg::partitioned_tlas const& ptlas,
                                                                    sg::ptlas_indirect_operations const& ops,
                                                                    bool is_first,
                                                                    cc::vector<sg::blas_table_handle>&& released)
{
    auto const& dx = static_cast<dx12_partitioned_tlas const&>(ptlas);
    ComPtr<ID3D12CommandListRaytracing2> list;
    [[maybe_unused]] HRESULT const list_hr = _list.As(&list);
    CC_ASSERT(SUCCEEDED(list_hr) && list, "update_indirect: the command list has no ID3D12CommandListRaytracing2");

    struct kind
    {
        sg::indirect_records const* records;
        D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TYPE type;
        isize stride;
    };
    kind const kinds[] = {
        {&ops.writes, D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TYPE_WRITE_INSTANCE, isize(sizeof(sg::gpu_ptlas_write))},
        {&ops.updates, D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TYPE_UPDATE_INSTANCE, isize(sizeof(sg::gpu_ptlas_update))},
        {&ops.translations, D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TYPE_TRANSLATE_PARTITION,
         isize(sizeof(sg::gpu_ptlas_translation))},
    };
    static_assert(sizeof(sg::gpu_ptlas_write) >= sizeof(D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS));
    static_assert(sizeof(sg::gpu_ptlas_update) == sizeof(D3D12_RTAS_PARTITIONED_TLAS_OPERATION_UPDATE_INSTANCE_ARGS));
    static_assert(sizeof(sg::gpu_ptlas_translation)
                  == sizeof(D3D12_RTAS_PARTITIONED_TLAS_OPERATION_TRANSLATE_PARTITION_ARGS));

    // The operations with no count yet, each counted below by a copy of the count its shader wrote.
    auto operations = cc::vector<D3D12_RTAS_PARTITIONED_TLAS_OPERATION>();
    auto counted = cc::vector<sg::indirect_records const*>();
    auto read = cc::vector<dx12_buffer_handle>();
    for (auto const& k : kinds)
    {
        if (k.records->max_count == 0)
            continue;
        auto records = std::dynamic_pointer_cast<dx12_buffer const>(k.records->records);
        CC_ASSERT(records != nullptr, "update_indirect: a records buffer is not a dx12 buffer");
        operations.push_back({.Type = k.type,
                              .ArgCount = 0,
                              .ArgData = {.StartAddress = records->gpu_virtual_address()
                                                        + D3D12_GPU_VIRTUAL_ADDRESS(k.records->offset_in_bytes),
                                          .StrideInBytes = UINT64(k.stride)}});
        counted.push_back(k.records);
        read.push_back(cc::move(records));
    }

    auto const operation_size = isize(sizeof(D3D12_RTAS_PARTITIONED_TLAS_OPERATION));
    auto const count_at = isize(operations.size()) * operation_size;
    auto const total = count_at + isize(sizeof(UINT));
    auto const input_raw = _ctx.transient.create_raw_buffer(
        total, sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto const input = std::dynamic_pointer_cast<dx12_buffer const>(input_raw);
    CC_ASSERT(input != nullptr, "update_indirect: the input buffer is not a dx12 buffer");
    auto bytes = cc::vector<byte>::create_filled(total, byte(0));
    if (!operations.empty())
        cc::memcpy(bytes.data(), operations.data(), size_t(count_at));
    auto const operation_count = UINT(operations.size());
    cc::memcpy(bytes.data() + count_at, &operation_count, sizeof(operation_count));
    upload_bytes_to_buffer(input_raw, cc::span<byte const>(bytes), 0);
    for (auto i = isize(0); i < counted.size(); ++i)
        copy_buffer_region(counted[i]->count, input_raw, counted[i]->count_offset_in_bytes,
                           i * operation_size + isize(offsetof(D3D12_RTAS_PARTITIONED_TLAS_OPERATION, ArgCount)),
                           isize(sizeof(UINT)));

    auto const scratch_size = dx.build_scratch_size_in_bytes();
    auto const scratch_raw = _ctx.transient.create_raw_buffer(scratch_size > 0 ? scratch_size : isize(1),
                                                              sg::buffer_usage::readwrite_buffer);
    auto const scratch = std::dynamic_pointer_cast<dx12_buffer const>(scratch_raw);
    CC_ASSERT(scratch != nullptr, "update_indirect: the scratch is not a dx12 buffer");

    // The structure in place, the operations and the records as shader reads, and every blas a table lists.
    track_buffer_access(dx._dx12_storage, sg::pipeline_stage_flag::accel_build,
                        sg::access_flag::accel_read | sg::access_flag::accel_write);
    track_buffer_access(scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(input, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    for (auto const& r : read)
        track_buffer_access(r, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    for (auto const& table : ops.tables)
        for (auto const& blas : table->blases())
            track_buffer_access(dx12_blas_of(blas)._dx12_storage, sg::pipeline_stage_flag::accel_build,
                                sg::access_flag::accel_read);
    flush_barriers();

    auto const ptlas_inputs = inputs_of(ptlas.description());
    auto const data = D3D12_RTAS_PARTITIONED_TLAS_OPERATION_DATA{
        .SourceAccelerationStructureData = is_first ? 0 : dx._dx12_storage->gpu_virtual_address(),
        .DestAccelerationStructureData = dx._dx12_storage->gpu_virtual_address(),
        .ScratchAccelerationStructureData = scratch->gpu_virtual_address(),
        .IndirectPartitionedTlasOpCount = input->gpu_virtual_address() + D3D12_GPU_VIRTUAL_ADDRESS(count_at),
        .IndirectPartitionedTlasOps = input->gpu_virtual_address(),
    };
    auto const desc = D3D12_RTAS_OPERATION_DESC{
        .Inputs = {.Type = D3D12_RTAS_OPERATION_TYPE_PARTITIONED_TLAS, .pPartitionedTLASInputsDesc = &ptlas_inputs},
        .pPartitionedTlasOperationData = &data};
    {
        sg::impl::raytracing_driver_hold const driver_guard;
        list->ExecuteIndirectRTASOperations(1, &desc, D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
    }

    // the tables the structure let go of, the records and the structure, until this list has run
    for (auto& table : released)
        keep_bound(cc::move(table));
    for (auto const& k : kinds)
        if (k.records->max_count > 0)
        {
            keep_bound(k.records->records);
            keep_bound(k.records->count);
        }
    keep_bound(ptlas.shared_from_this());
}
#else
cc::result<sg::partitioned_tlas_handle> dx12_context::try_create_partitioned_tlas(sg::partitioned_tlas_description const&)
{
    return cc::error("create_partitioned_tlas: dx12 builds one only against the preview Agility SDK "
                     "(SC_AGILITY_PREVIEW)");
}

void dx12_command_list::raytracing_update_partitioned_tlas(sg::partitioned_tlas const&,
                                                           sg::ptlas_operations const&,
                                                           bool,
                                                           cc::vector<sg::blas_handle>&&)
{
    CC_ASSERT(false, "update: dx12 updates a partitioned tlas only against the preview Agility SDK");
}

void dx12_command_list::raytracing_update_partitioned_tlas_indirect(sg::partitioned_tlas const&,
                                                                    sg::ptlas_indirect_operations const&,
                                                                    bool,
                                                                    cc::vector<sg::blas_table_handle>&&)
{
    CC_ASSERT(false, "update_indirect: dx12 updates a partitioned tlas only against the preview Agility SDK");
}
#endif
} // namespace sg::backend::dx12
