// vulkan partitioned tlas: VK_NV_partitioned_acceleration_structure behind ctx.persistent.create_partitioned_tlas and
// cmd.raytracing.update.
// See libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md.

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh> // offsetof
#include <shaped-graphics/backends/vulkan/vulkan_acceleration_structure.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/raytracing/blas_table.hh>
#include <shaped-graphics/raytracing/partitioned_tlas.hh>

namespace sg::backend::vulkan
{
namespace
{
/// Every address the build reads or writes a structure at, and its scratch.
constexpr auto k_structure_alignment = VkDeviceAddress(256);

[[nodiscard]] VkPartitionedAccelerationStructureInstancesInputNV instances_input_of(
    sg::partitioned_tlas_description const& d,
    VkPartitionedAccelerationStructureFlagsNV const* flags)
{
    auto build_flags = VkBuildAccelerationStructureFlagsKHR(0);
    if (d.flags.has(sg::accel_build_flag::fast_trace))
        build_flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (d.flags.has(sg::accel_build_flag::fast_build))
        build_flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    return {
        .sType = VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCES_INPUT_NV,
        .pNext = const_cast<VkPartitionedAccelerationStructureFlagsNV*>(flags),
        .flags = build_flags,
        .instanceCount = u32(d.instance_capacity),
        .maxInstancePerPartitionCount = u32(d.max_instances_per_partition),
        .partitionCount = u32(d.partition_count),
        .maxInstanceInGlobalPartitionCount = u32(d.max_global_instances),
    };
}

[[nodiscard]] u32 partition_index_of(int partition)
{
    return partition == sg::global_partition ? VK_PARTITIONED_ACCELERATION_STRUCTURE_PARTITION_INDEX_GLOBAL_NV
                                             : u32(partition);
}

[[nodiscard]] VkPartitionedAccelerationStructureInstanceFlagsNV instance_flags_of(sg::tlas_instance const& inst)
{
    auto flags = VkPartitionedAccelerationStructureInstanceFlagsNV(0);
    switch (inst.cull_mode)
    {
    case sg::instance_cull_mode::back:
        break;
    case sg::instance_cull_mode::front:
        flags |= VK_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCE_FLAG_TRIANGLE_FLIP_FACING_BIT_NV;
        break;
    case sg::instance_cull_mode::none:
        flags |= VK_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCE_FLAG_TRIANGLE_FACING_CULL_DISABLE_BIT_NV;
        break;
    }
    if (inst.opaque_override.has_value())
        flags |= inst.opaque_override.value()
                   ? VK_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCE_FLAG_FORCE_OPAQUE_BIT_NV
                   : VK_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCE_FLAG_FORCE_NO_OPAQUE_BIT_NV;
    return flags;
}

[[nodiscard]] vulkan_blas const& vulkan_blas_of(sg::blas_handle const& blas)
{
    auto const* const vk = dynamic_cast<vulkan_blas const*>(blas.get());
    CC_ASSERT(vk != nullptr, "update: a blas is not a vulkan blas");
    return *vk;
}

[[nodiscard]] isize aligned(isize at, isize alignment)
{
    return (at + alignment - 1) / alignment * alignment;
}
} // namespace

cc::result<sg::partitioned_tlas_handle> vulkan_context::try_create_partitioned_tlas(
    sg::partitioned_tlas_description const& desc)
{
    auto const translation = VkPartitionedAccelerationStructureFlagsNV{
        .sType = VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_FLAGS_NV,
        .enablePartitionTranslation = desc.allows_partition_translation ? VK_TRUE : VK_FALSE,
    };
    auto const input = instances_input_of(desc, &translation);
    auto sizes = VkAccelerationStructureBuildSizesInfoKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    _raytracing_functions.get_partitioned_build_sizes(_device, &input, &sizes);
    if (sizes.accelerationStructureSize == 0)
        return cc::error("create_partitioned_tlas: the device sized the structure at zero bytes");

    auto storage_raw = persistent.create_raw_buffer(
        isize(sizes.accelerationStructureSize) + isize(k_structure_alignment), sg::buffer_usage::accel_structure_storage);
    auto storage = std::dynamic_pointer_cast<vulkan_buffer const>(storage_raw);
    CC_ASSERT(storage != nullptr, "the structure's storage is not a vulkan buffer");
    // the build takes the structure at an aligned address, which an allocation need not start at
    auto const address = (storage->_device_address + k_structure_alignment - 1) & ~(k_structure_alignment - 1);
    return sg::partitioned_tlas_handle(std::make_shared<vulkan_partitioned_tlas>(
        desc, cc::move(storage), address, isize(sizes.accelerationStructureSize), isize(sizes.buildScratchSize),
        isize(sizes.updateScratchSize)));
}

u64 vulkan_context::blas_address_of(sg::blas const& blas) const
{
    auto const* const vk = dynamic_cast<vulkan_blas const*>(&blas);
    CC_ASSERT(vk != nullptr, "blas_address_of: a blas is not a vulkan blas");
    return u64(vk->_address);
}

void vulkan_command_list::raytracing_update_partitioned_tlas(sg::partitioned_tlas const& ptlas,
                                                             sg::ptlas_operations const& ops,
                                                             bool is_first,
                                                             cc::vector<sg::blas_handle>&& released)
{
    auto const& vk = static_cast<vulkan_partitioned_tlas const&>(ptlas);
    auto const& d = ptlas.description();

    // The records, as VK_NV_partitioned_acceleration_structure lays them out, and the blases they read.
    auto writes = cc::vector<VkPartitionedAccelerationStructureWriteInstanceDataNV>();
    auto updates = cc::vector<VkPartitionedAccelerationStructureUpdateInstanceDataNV>();
    auto translations = cc::vector<VkPartitionedAccelerationStructureWritePartitionTranslationDataNV>();
    auto read_storage = cc::vector<vulkan_buffer_handle>();
    for (auto const& w : ops.writes)
    {
        auto const& blas = vulkan_blas_of(w.instance.blas);
        auto record = VkPartitionedAccelerationStructureWriteInstanceDataNV{
            .instanceID = w.instance.instance_id,
            .instanceMask = w.instance.mask,
            .instanceContributionToHitGroupIndex = w.instance.hit_group_offset,
            .instanceFlags = instance_flags_of(w.instance),
            .instanceIndex = u32(w.index),
            .partitionIndex = partition_index_of(w.partition),
            .accelerationStructure = blas._address,
        };
        for (int k = 0; k < 12; ++k) // both row-major 3x4: element k maps straight through
            (&record.transform.matrix[0][0])[k] = w.instance.transform[k];
        writes.push_back(record);
        read_storage.push_back(blas._vulkan_storage);
    }
    // a clear writes the slot with no structure, which leaves it in no partition a ray reaches
    for (auto const index : ops.clears)
        writes.push_back({.instanceIndex = u32(index),
                          .partitionIndex = VK_PARTITIONED_ACCELERATION_STRUCTURE_PARTITION_INDEX_GLOBAL_NV,
                          .accelerationStructure = 0});
    for (auto const& u : ops.updates)
    {
        auto const& blas = vulkan_blas_of(u.blas);
        updates.push_back({.instanceIndex = u32(u.index),
                           .instanceContributionToHitGroupIndex = u.hit_group_offset,
                           .accelerationStructure = blas._address});
        read_storage.push_back(blas._vulkan_storage);
    }
    for (auto const& t : ops.translations)
        translations.push_back({.partitionIndex = partition_index_of(t.partition),
                                .partitionTranslation = {t.translation[0], t.translation[1], t.translation[2]}});

    // One transient buffer: the commands, their count, then each kind's records, each at its own alignment.
    auto const commands_at = isize(0);
    auto const command_count = isize(!writes.empty()) + isize(!updates.empty()) + isize(!translations.empty());
    auto const count_at
        = commands_at + command_count * isize(sizeof(VkBuildPartitionedAccelerationStructureIndirectCommandNV));
    auto const writes_at = aligned(count_at + isize(sizeof(u32)), 16);
    auto const updates_at = aligned(writes_at + writes.size() * isize(sizeof(writes[0])), 16);
    auto const translations_at = aligned(updates_at + updates.size() * isize(sizeof(updates[0])), 16);
    auto const total = translations_at + translations.size() * isize(sizeof(translations[0]));

    auto const input_raw = _ctx.transient.create_raw_buffer(
        total, sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto const input = std::dynamic_pointer_cast<vulkan_buffer const>(input_raw);
    CC_ASSERT(input != nullptr, "the update's input buffer is not a vulkan buffer");
    auto const at = [&](isize offset) { return input->_device_address + VkDeviceAddress(offset); };

    auto commands = cc::vector<VkBuildPartitionedAccelerationStructureIndirectCommandNV>();
    if (!writes.empty())
        commands.push_back({.opType = VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_WRITE_INSTANCE_NV,
                            .argCount = u32(writes.size()),
                            .argData = {.startAddress = at(writes_at), .strideInBytes = sizeof(writes[0])}});
    if (!updates.empty())
        commands.push_back({.opType = VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_UPDATE_INSTANCE_NV,
                            .argCount = u32(updates.size()),
                            .argData = {.startAddress = at(updates_at), .strideInBytes = sizeof(updates[0])}});
    if (!translations.empty())
        commands.push_back({.opType = VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_WRITE_PARTITION_TRANSLATION_NV,
                            .argCount = u32(translations.size()),
                            .argData = {.startAddress = at(translations_at), .strideInBytes = sizeof(translations[0])}});

    auto bytes = cc::vector<byte>::create_filled(total, byte(0));
    auto const put = [&](isize offset, cc::span<byte const> part)
    { cc::memcpy(bytes.data() + offset, part.data(), size_t(part.size())); };
    put(commands_at, cc::as_bytes(cc::span<VkBuildPartitionedAccelerationStructureIndirectCommandNV const>(commands)));
    auto const count = u32(commands.size());
    put(count_at, cc::as_bytes(cc::span<u32 const>(&count, 1)));
    put(writes_at, cc::as_bytes(cc::span<VkPartitionedAccelerationStructureWriteInstanceDataNV const>(writes)));
    put(updates_at, cc::as_bytes(cc::span<VkPartitionedAccelerationStructureUpdateInstanceDataNV const>(updates)));
    put(translations_at,
        cc::as_bytes(cc::span<VkPartitionedAccelerationStructureWritePartitionTranslationDataNV const>(translations)));
    upload_bytes_to_buffer(input_raw, cc::span<byte const>(bytes), 0);

    // every build takes the build scratch, an update in place too (VUID-vkCmdBuildPartitionedAccelerationStructuresNV-pBuildInfo-10541)
    auto const scratch_size = vk.build_scratch_size_in_bytes();
    auto const scratch_raw = _ctx.transient.create_raw_buffer(
        (scratch_size > 0 ? scratch_size : isize(1)) + isize(k_structure_alignment), sg::buffer_usage::readwrite_buffer);
    auto const scratch = std::dynamic_pointer_cast<vulkan_buffer const>(scratch_raw);
    CC_ASSERT(scratch != nullptr, "the update's scratch is not a vulkan buffer");
    auto const scratch_address = (scratch->_device_address + k_structure_alignment - 1) & ~(k_structure_alignment - 1);

    // The structure is read and written in place; the records are indirect commands and their arguments; each blas a
    // write or an update places is read as a structure.
    track_buffer_access(*vk._vulkan_storage, sg::pipeline_stage_flag::accel_build,
                        sg::access_flag::accel_read | sg::access_flag::accel_write);
    track_buffer_access(*scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(*input, sg::pipeline_stage_flag::accel_build,
                        sg::access_flag::indirect_read | sg::access_flag::shader_read);
    for (auto const& s : read_storage)
        track_buffer_access(*s, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_read);
    flush_barriers();

    auto const translation = VkPartitionedAccelerationStructureFlagsNV{
        .sType = VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_FLAGS_NV,
        .enablePartitionTranslation = d.allows_partition_translation ? VK_TRUE : VK_FALSE,
    };
    auto const info = VkBuildPartitionedAccelerationStructureInfoNV{
        .sType = VK_STRUCTURE_TYPE_BUILD_PARTITIONED_ACCELERATION_STRUCTURE_INFO_NV,
        .input = instances_input_of(d, &translation),
        .srcAccelerationStructureData = is_first ? 0 : vk._address,
        .dstAccelerationStructureData = vk._address,
        .scratchData = scratch_address,
        .srcInfos = at(commands_at),
        .srcInfosCount = at(count_at),
    };
    _ctx._raytracing_functions.cmd_build_partitioned(_buffer, &info);

    // the blases no slot names any more, until this list has run; and the structure itself
    for (auto& blas : released)
        keep_bound(cc::move(blas));
    keep_bound(ptlas.shared_from_this());
}

void vulkan_command_list::raytracing_update_partitioned_tlas_indirect(sg::partitioned_tlas const& ptlas,
                                                                      sg::ptlas_indirect_operations const& ops,
                                                                      bool is_first,
                                                                      cc::vector<sg::blas_table_handle>&& released)
{
    auto const& vk = static_cast<vulkan_partitioned_tlas const&>(ptlas);
    auto const& d = ptlas.description();

    struct kind
    {
        sg::indirect_records const* records;
        VkPartitionedAccelerationStructureOpTypeNV type;
        isize stride;
    };
    kind const kinds[] = {
        {&ops.writes, VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_WRITE_INSTANCE_NV, isize(sizeof(sg::gpu_ptlas_write))},
        {&ops.updates, VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_UPDATE_INSTANCE_NV,
         isize(sizeof(sg::gpu_ptlas_update))},
        {&ops.translations, VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_WRITE_PARTITION_TRANSLATION_NV,
         isize(sizeof(sg::gpu_ptlas_translation))},
    };
    static_assert(sizeof(sg::gpu_ptlas_write) >= sizeof(VkPartitionedAccelerationStructureWriteInstanceDataNV));
    static_assert(sizeof(sg::gpu_ptlas_update) == sizeof(VkPartitionedAccelerationStructureUpdateInstanceDataNV));
    static_assert(sizeof(sg::gpu_ptlas_translation)
                  == sizeof(VkPartitionedAccelerationStructureWritePartitionTranslationDataNV));

    // The commands with no count yet, each counted below by a copy of the count its shader wrote.
    auto commands = cc::vector<VkBuildPartitionedAccelerationStructureIndirectCommandNV>();
    auto counted = cc::vector<sg::indirect_records const*>();
    auto read = cc::vector<vulkan_buffer_handle>();
    for (auto const& k : kinds)
    {
        if (k.records->max_count == 0)
            continue;
        auto records = std::dynamic_pointer_cast<vulkan_buffer const>(k.records->records);
        CC_ASSERT(records != nullptr, "update_indirect: a records buffer is not a vulkan buffer");
        commands.push_back(
            {.opType = k.type,
             .argCount = 0,
             .argData = {.startAddress = records->_device_address + VkDeviceAddress(k.records->offset_in_bytes),
                         .strideInBytes = VkDeviceSize(k.stride)}});
        counted.push_back(k.records);
        read.push_back(cc::move(records));
    }

    auto const command_size = isize(sizeof(VkBuildPartitionedAccelerationStructureIndirectCommandNV));
    auto const count_at = isize(commands.size()) * command_size;
    auto const total = count_at + isize(sizeof(u32));
    auto const input_raw = _ctx.transient.create_raw_buffer(
        total, sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto const input = std::dynamic_pointer_cast<vulkan_buffer const>(input_raw);
    CC_ASSERT(input != nullptr, "update_indirect: the input buffer is not a vulkan buffer");
    auto bytes = cc::vector<byte>::create_filled(total, byte(0));
    if (!commands.empty())
        cc::memcpy(bytes.data(), commands.data(), size_t(count_at));
    auto const command_count = u32(commands.size());
    cc::memcpy(bytes.data() + count_at, &command_count, sizeof(command_count));
    upload_bytes_to_buffer(input_raw, cc::span<byte const>(bytes), 0);
    for (auto i = isize(0); i < counted.size(); ++i)
        copy_buffer_region(
            counted[i]->count, input_raw, counted[i]->count_offset_in_bytes,
            i * command_size + isize(offsetof(VkBuildPartitionedAccelerationStructureIndirectCommandNV, argCount)),
            isize(sizeof(u32)));

    // every build takes the build scratch, an update in place too (VUID-vkCmdBuildPartitionedAccelerationStructuresNV-pBuildInfo-10541)
    auto const scratch_size = vk.build_scratch_size_in_bytes();
    auto const scratch_raw = _ctx.transient.create_raw_buffer(
        (scratch_size > 0 ? scratch_size : isize(1)) + isize(k_structure_alignment), sg::buffer_usage::readwrite_buffer);
    auto const scratch = std::dynamic_pointer_cast<vulkan_buffer const>(scratch_raw);
    CC_ASSERT(scratch != nullptr, "update_indirect: the scratch is not a vulkan buffer");
    auto const scratch_address = (scratch->_device_address + k_structure_alignment - 1) & ~(k_structure_alignment - 1);

    // The structure in place, the commands and the records as the build reads them, and every blas a table lists.
    track_buffer_access(*vk._vulkan_storage, sg::pipeline_stage_flag::accel_build,
                        sg::access_flag::accel_read | sg::access_flag::accel_write);
    track_buffer_access(*scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(*input, sg::pipeline_stage_flag::accel_build,
                        sg::access_flag::indirect_read | sg::access_flag::shader_read);
    for (auto const& r : read)
        track_buffer_access(*r, sg::pipeline_stage_flag::accel_build,
                            sg::access_flag::indirect_read | sg::access_flag::shader_read);
    for (auto const& table : ops.tables)
        for (auto const& blas : table->blases())
            track_buffer_access(*vulkan_blas_of(blas)._vulkan_storage, sg::pipeline_stage_flag::accel_build,
                                sg::access_flag::accel_read);
    flush_barriers();

    auto const translation = VkPartitionedAccelerationStructureFlagsNV{
        .sType = VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_FLAGS_NV,
        .enablePartitionTranslation = d.allows_partition_translation ? VK_TRUE : VK_FALSE,
    };
    auto const info = VkBuildPartitionedAccelerationStructureInfoNV{
        .sType = VK_STRUCTURE_TYPE_BUILD_PARTITIONED_ACCELERATION_STRUCTURE_INFO_NV,
        .input = instances_input_of(d, &translation),
        .srcAccelerationStructureData = is_first ? 0 : vk._address,
        .dstAccelerationStructureData = vk._address,
        .scratchData = scratch_address,
        .srcInfos = input->_device_address,
        .srcInfosCount = input->_device_address + VkDeviceAddress(count_at),
    };
    _ctx._raytracing_functions.cmd_build_partitioned(_buffer, &info);

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
} // namespace sg::backend::vulkan
