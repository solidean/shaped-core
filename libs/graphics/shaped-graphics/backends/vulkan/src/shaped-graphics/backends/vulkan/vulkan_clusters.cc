// vulkan clustered geometry: VK_NV_cluster_acceleration_structure behind cmd.raytracing.build_clusters and
// build_cluster_blas.
// See libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh> // offsetof
#include <shaped-graphics/backends/vulkan/vulkan_acceleration_structure.hh>
#include <shaped-graphics/backends/vulkan/vulkan_barrier.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/raytracing/clusters.hh>

namespace sg::backend::vulkan
{
namespace
{
[[nodiscard]] VkBuildAccelerationStructureFlagsKHR build_flags_of(sg::accel_build_flags flags)
{
    auto out = VkBuildAccelerationStructureFlagsKHR(0);
    if (flags.has(sg::accel_build_flag::fast_trace))
        out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (flags.has(sg::accel_build_flag::fast_build))
        out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    return out;
}

[[nodiscard]] VkDeviceAddress aligned(VkDeviceAddress at, VkDeviceAddress alignment)
{
    return alignment <= 1 ? at : (at + alignment - 1) / alignment * alignment;
}

[[nodiscard]] vulkan_buffer_handle vulkan_buffer_of(sg::raw_buffer_handle const& buffer, char const* what)
{
    auto vk = std::dynamic_pointer_cast<vulkan_buffer const>(buffer);
    CC_ASSERTF(vk != nullptr, "{} is not a vulkan buffer", what);
    return vk;
}

/// A pool's limits, as both its sizing and every build into it state them.
[[nodiscard]] VkClusterAccelerationStructureTriangleClusterInputNV triangle_input_of(sg::cluster_pool_description const& d)
{
    return {
        .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV,
        .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
        .maxGeometryIndexValue = u32(d.max_geometry_index),
        .maxClusterUniqueGeometryCount = 1,
        .maxClusterTriangleCount = u32(d.max_triangles_per_cluster),
        .maxClusterVertexCount = u32(d.max_vertices_per_cluster),
        .maxTotalTriangleCount = u32(d.max_total_triangles),
        .maxTotalVertexCount = u32(d.max_total_vertices),
        .minPositionTruncateBitCount = 0,
    };
}

[[nodiscard]] VkClusterAccelerationStructureInputInfoNV pool_input_of(
    sg::cluster_pool_description const& d,
    VkClusterAccelerationStructureTriangleClusterInputNV const* triangles)
{
    return {
        .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV,
        .maxAccelerationStructureCount = u32(d.max_clusters),
        .flags = build_flags_of(d.flags),
        .opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV,
        .opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV,
        .opInput = {.pTriangleClusters = const_cast<VkClusterAccelerationStructureTriangleClusterInputNV*>(triangles)},
    };
}

/// The storage and the addresses of a set or a pool.
[[nodiscard]] vulkan_clusters vulkan_clusters_of(sg::cluster_set const& clusters)
{
    if (auto const* const set = dynamic_cast<vulkan_cluster_set const*>(&clusters))
        return {.storage = set->_vulkan_storage, .addresses = set->_addresses};
    auto const* const pool = dynamic_cast<vulkan_cluster_pool const*>(&clusters);
    CC_ASSERT(pool != nullptr, "the clusters are no vulkan cluster set or pool");
    return {.storage = pool->_vulkan_storage, .addresses = pool->_addresses};
}
} // namespace

u64 vulkan_context::buffer_address_of(sg::raw_buffer const& buffer) const
{
    auto const* const vk = dynamic_cast<vulkan_buffer const*>(&buffer);
    CC_ASSERT(vk != nullptr, "buffer_address_of: a buffer is not a vulkan buffer");
    return u64(vk->_device_address);
}

cc::result<sg::cluster_pool_handle> vulkan_context::try_create_cluster_pool(sg::cluster_pool_description const& desc)
{
    if (!is_clustered_geometry_supported())
        return cc::error("create_cluster_pool: this device has no VK_NV_cluster_acceleration_structure");
    auto const& props = cluster_properties();
    if (u32(desc.max_triangles_per_cluster) > props.maxTrianglesPerCluster
        || u32(desc.max_vertices_per_cluster) > props.maxVerticesPerCluster)
        return cc::error("create_cluster_pool: a cluster exceeds the device's limits");
    auto const triangles = triangle_input_of(desc);
    auto const input = pool_input_of(desc, &triangles);
    auto sizes = VkAccelerationStructureBuildSizesInfoKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    _raytracing_functions.get_cluster_build_sizes(_device, &input, &sizes);
    if (sizes.accelerationStructureSize == 0)
        return cc::error("create_cluster_pool: the device sized the pool at zero bytes");
    auto storage = vulkan_buffer_of(
        persistent.create_raw_buffer(isize(sizes.accelerationStructureSize) + isize(props.clusterByteAlignment),
                                     sg::buffer_usage::accel_structure_storage),
        "create_cluster_pool: the storage");
    auto addresses = vulkan_buffer_of(
        persistent.create_raw_buffer(isize(desc.max_clusters) * isize(sizeof(VkDeviceAddress)),
                                     sg::buffer_usage::readwrite_buffer | sg::buffer_usage::accel_structure_build_input),
        "create_cluster_pool: the address array");
    return sg::cluster_pool_handle(std::make_shared<vulkan_cluster_pool>(desc, isize(sizes.accelerationStructureSize),
                                                                         isize(sizes.buildScratchSize),
                                                                         cc::move(storage), cc::move(addresses)));
}

void vulkan_command_list::raytracing_build_clusters_indirect(sg::cluster_pool const& pool,
                                                             sg::cluster_indirect_build const& build)
{
    auto const& rt = _ctx._raytracing_functions;
    auto const& props = _ctx.cluster_properties();
    auto const& vk = static_cast<vulkan_cluster_pool const&>(pool);
    auto const& d = pool.description();
    auto const& r = build.clusters;
    auto const records = vulkan_buffer_of(r.records, "build_clusters_indirect: the records");
    auto const count = vulkan_buffer_of(r.count, "build_clusters_indirect: the count");
    auto const stride = isize(sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV));

    auto const scratch = vulkan_buffer_of(
        _ctx.transient.create_raw_buffer(vk._build_scratch_size_in_bytes + isize(props.clusterScratchByteAlignment),
                                         sg::buffer_usage::readwrite_buffer),
        "build_clusters_indirect: the scratch");

    // The pool is written in place, its addresses with it; the records and their count are what the build reads, and
    // so is every buffer a table lists, which the records name by address.
    track_buffer_access(*vk._vulkan_storage, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    track_buffer_access(*vk._addresses, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    track_buffer_access(*scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(*records, sg::pipeline_stage_flag::accel_build,
                        sg::access_flag::indirect_read | sg::access_flag::shader_read);
    if (count != records)
        track_buffer_access(*count, sg::pipeline_stage_flag::accel_build,
                            sg::access_flag::indirect_read | sg::access_flag::shader_read);
    for (auto const& table : build.tables)
        for (auto const& b : table->buffers())
            track_buffer_access(*vulkan_buffer_of(b, "build_clusters_indirect: a table's buffer"),
                                sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    flush_barriers();
    auto after_traces = make_empty_memory_barrier();
    merge_into_memory_barrier(after_traces, make_buffer_barrier(VK_NULL_HANDLE, sg::impl::pool_rebuild_after_traces()));
    auto const globals = cc::span<VkMemoryBarrier2 const>(&after_traces, 1);
    count_barriers(_stats, cc::span<VkBufferMemoryBarrier2 const>(), cc::span<VkImageMemoryBarrier2 const>(), globals);
    submit_barriers(_buffer, {}, {}, globals);

    auto const triangles = triangle_input_of(d);
    auto const commands = VkClusterAccelerationStructureCommandsInfoNV{
        .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV,
        .input = pool_input_of(d, &triangles),
        .dstImplicitData = aligned(vk._vulkan_storage->_device_address, props.clusterByteAlignment),
        .scratchData = aligned(scratch->_device_address, props.clusterScratchByteAlignment),
        .dstAddressesArray = {.deviceAddress = vk._addresses->_device_address,
                              .stride = sizeof(VkDeviceAddress),
                              .size = VkDeviceSize(isize(d.max_clusters) * isize(sizeof(VkDeviceAddress)))},
        .dstSizesArray = {},
        .srcInfosArray = {.deviceAddress = records->_device_address + VkDeviceAddress(r.offset_in_bytes),
                          .stride = VkDeviceSize(stride),
                          .size = VkDeviceSize(isize(r.max_count) * stride)},
        .srcInfosCount = count->_device_address + VkDeviceAddress(r.count_offset_in_bytes),
        .addressResolutionFlags = 0,
    };
    rt.cmd_build_clusters(_buffer, &commands);

    keep_bound(r.records);
    keep_bound(r.count);
    for (auto const& table : build.tables)
        keep_bound(table);
    keep_bound(pool.shared_from_this());
}

sg::cluster_set_handle vulkan_command_list::raytracing_build_clusters(cc::span<sg::cluster_triangles const> clusters,
                                                                      sg::accel_build_flags flags)
{
    auto const& rt = _ctx._raytracing_functions;
    auto const& props = _ctx.cluster_properties();

    auto max_triangles = 0;
    auto max_vertices = 0;
    auto max_geometry_index = 0;
    auto total_triangles = isize(0);
    auto total_vertices = isize(0);
    for (auto const& c : clusters)
    {
        CC_ASSERTF(
            u32(c.triangle_count) <= props.maxTrianglesPerCluster && u32(c.vertex_count) <= props.maxVerticesPerCluster,
            "build_clusters: a cluster of {} triangles over {} vertices exceeds the device's {} and {}",
            c.triangle_count, c.vertex_count, props.maxTrianglesPerCluster, props.maxVerticesPerCluster);
        max_triangles = cc::max(max_triangles, c.triangle_count);
        max_vertices = cc::max(max_vertices, c.vertex_count);
        max_geometry_index = cc::max(max_geometry_index, c.geometry_index);
        total_triangles += c.triangle_count;
        total_vertices += c.vertex_count;
    }

    auto const triangle_input = VkClusterAccelerationStructureTriangleClusterInputNV{
        .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV,
        .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
        .maxGeometryIndexValue = u32(max_geometry_index),
        .maxClusterUniqueGeometryCount = 1,
        .maxClusterTriangleCount = u32(max_triangles),
        .maxClusterVertexCount = u32(max_vertices),
        .maxTotalTriangleCount = u32(total_triangles),
        .maxTotalVertexCount = u32(total_vertices),
        .minPositionTruncateBitCount = 0,
    };
    auto const input = VkClusterAccelerationStructureInputInfoNV{
        .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV,
        .maxAccelerationStructureCount = u32(clusters.size()),
        .flags = build_flags_of(flags),
        .opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV,
        .opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV,
        .opInput
        = {.pTriangleClusters = const_cast<VkClusterAccelerationStructureTriangleClusterInputNV*>(&triangle_input)},
    };
    auto sizes = VkAccelerationStructureBuildSizesInfoKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt.get_cluster_build_sizes(_ctx._device, &input, &sizes);
    CC_ASSERT(sizes.accelerationStructureSize > 0, "build_clusters: the device sized the clusters at zero bytes");

    // The clusters, packed where the driver chooses within the storage, and the address it gives each.
    auto const storage = vulkan_buffer_of(
        _ctx.persistent.create_raw_buffer(isize(sizes.accelerationStructureSize) + isize(props.clusterByteAlignment),
                                          sg::buffer_usage::accel_structure_storage),
        "build_clusters: the storage");
    auto const addresses
        = vulkan_buffer_of(_ctx.persistent.create_raw_buffer(
                               clusters.size() * isize(sizeof(VkDeviceAddress)),
                               sg::buffer_usage::readwrite_buffer | sg::buffer_usage::accel_structure_build_input),
                           "build_clusters: the address array");

    // Each cluster's record, as VK_NV_cluster_acceleration_structure lays it out, then their count.
    auto infos = cc::vector<VkClusterAccelerationStructureBuildTriangleClusterInfoNV>();
    auto inputs = cc::vector<vulkan_buffer_handle>();
    for (auto const& c : clusters)
    {
        auto const vertices = vulkan_buffer_of(c.vertices, "build_clusters: a vertex buffer");
        auto const indices = vulkan_buffer_of(c.indices, "build_clusters: an index buffer");
        auto const is_wide = c.index_type == sg::index_format::uint32;
        auto info = VkClusterAccelerationStructureBuildTriangleClusterInfoNV{
            .clusterID = c.cluster_id,
            .clusterFlags = 0,
            .triangleCount = u32(c.triangle_count),
            .vertexCount = u32(c.vertex_count),
            .positionTruncateBitCount = 0,
            .indexType = u32(is_wide ? VK_CLUSTER_ACCELERATION_STRUCTURE_INDEX_FORMAT_32BIT_NV
                                     : VK_CLUSTER_ACCELERATION_STRUCTURE_INDEX_FORMAT_16BIT_NV),
            .opacityMicromapIndexType = 0,
            .baseGeometryIndexAndGeometryFlags
            = {.geometryIndex = u32(c.geometry_index),
               .reserved = 0,
               .geometryFlags = c.is_opaque ? u32(VK_CLUSTER_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT_NV) : 0u},
            .indexBufferStride = u16(is_wide ? 4 : 2),
            .vertexBufferStride = u16(c.vertex_stride_in_bytes),
            .indexBuffer = indices->_device_address + VkDeviceAddress(c.index_offset_in_bytes),
            .vertexBuffer = vertices->_device_address + VkDeviceAddress(c.vertex_offset_in_bytes),
        };
        infos.push_back(info);
        inputs.push_back(vertices);
        inputs.push_back(indices);
    }
    auto const info_size = isize(sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV));
    auto const count_at = isize(aligned(VkDeviceAddress(infos.size() * info_size), 16));
    auto bytes = cc::vector<byte>::create_filled(count_at + isize(sizeof(u32)), byte(0));
    cc::memcpy(bytes.data(), infos.data(), size_t(infos.size() * info_size));
    auto const count = u32(infos.size());
    cc::memcpy(bytes.data() + count_at, &count, sizeof(count));
    auto const args_raw = _ctx.transient.create_raw_buffer(
        bytes.size(), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto const args = vulkan_buffer_of(args_raw, "build_clusters: the records");
    upload_bytes_to_buffer(args_raw, cc::span<byte const>(bytes), 0);

    auto const scratch = vulkan_buffer_of(
        _ctx.transient.create_raw_buffer(isize(sizes.buildScratchSize) + isize(props.clusterScratchByteAlignment),
                                         sg::buffer_usage::readwrite_buffer),
        "build_clusters: the scratch");

    track_buffer_access(*storage, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    track_buffer_access(*addresses, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    track_buffer_access(*scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(*args, sg::pipeline_stage_flag::accel_build,
                        sg::access_flag::indirect_read | sg::access_flag::shader_read);
    for (auto const& in : inputs)
        track_buffer_access(*in, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    flush_barriers();

    auto const commands = VkClusterAccelerationStructureCommandsInfoNV{
        .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV,
        .input = input,
        .dstImplicitData = aligned(storage->_device_address, props.clusterByteAlignment),
        .scratchData = aligned(scratch->_device_address, props.clusterScratchByteAlignment),
        .dstAddressesArray = {.deviceAddress = addresses->_device_address,
                              .stride = sizeof(VkDeviceAddress),
                              .size = VkDeviceSize(clusters.size() * isize(sizeof(VkDeviceAddress)))},
        .dstSizesArray = {},
        .srcInfosArray = {.deviceAddress = args->_device_address,
                          .stride = VkDeviceSize(info_size),
                          .size = VkDeviceSize(infos.size() * info_size)},
        .srcInfosCount = args->_device_address + VkDeviceAddress(count_at),
        .addressResolutionFlags = 0,
    };
    rt.cmd_build_clusters(_buffer, &commands);

    for (auto& in : inputs)
        keep_bound(cc::move(in));
    return std::make_shared<vulkan_cluster_set>(int(clusters.size()), max_geometry_index,
                                                isize(sizes.accelerationStructureSize), storage, addresses);
}

cc::vector<sg::blas_handle> vulkan_command_list::raytracing_build_cluster_blas(sg::cluster_set_handle const& clusters,
                                                                               cc::span<sg::cluster_range const> ranges,
                                                                               sg::accel_build_flags flags,
                                                                               int /*hit_record_stride*/)
{
    auto const& rt = _ctx._raytracing_functions;
    auto const& props = _ctx.cluster_properties();
    auto const set = vulkan_clusters_of(*clusters);

    auto const input_of = [](u32 total, u32 max_per)
    {
        return VkClusterAccelerationStructureClustersBottomLevelInputNV{
            .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV,
            .maxTotalClusterCount = total,
            .maxClusterCountPerAccelerationStructure = max_per,
        };
    };
    auto const info_of = [&](VkClusterAccelerationStructureClustersBottomLevelInputNV const* bottom, u32 count)
    {
        return VkClusterAccelerationStructureInputInfoNV{
            .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV,
            .maxAccelerationStructureCount = count,
            .flags = build_flags_of(flags),
            .opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV,
            .opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV,
            .opInput
            = {.pClustersBottomLevel = const_cast<VkClusterAccelerationStructureClustersBottomLevelInputNV*>(bottom)},
        };
    };

    // Each BLAS into storage of its own, sized for its own clusters, so the CPU knows the address a tlas names it by.
    auto total = u32(0);
    auto max_per = u32(0);
    auto blases = cc::vector<sg::blas_handle>();
    auto storages = cc::vector<vulkan_buffer_handle>();
    auto destinations = cc::vector<VkDeviceAddress>();
    auto records = cc::vector<VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV>();
    for (auto const& r : ranges)
    {
        total += u32(r.count);
        max_per = cc::max(max_per, u32(r.count));
        auto const one = input_of(u32(r.count), u32(r.count));
        auto const one_info = info_of(&one, 1);
        auto sizes = VkAccelerationStructureBuildSizesInfoKHR{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        rt.get_cluster_build_sizes(_ctx._device, &one_info, &sizes);
        CC_ASSERT(sizes.accelerationStructureSize > 0, "build_cluster_blas: the device sized a blas at zero bytes");
        auto const storage
            = vulkan_buffer_of(_ctx.persistent.create_raw_buffer(
                                   isize(sizes.accelerationStructureSize) + isize(props.clusterBottomLevelByteAlignment),
                                   sg::buffer_usage::accel_structure_storage),
                               "build_cluster_blas: a blas's storage");
        auto const address = aligned(storage->_device_address, props.clusterBottomLevelByteAlignment);
        destinations.push_back(address);
        records.push_back(
            {.clusterReferencesCount = u32(r.count),
             .clusterReferencesStride = sizeof(VkDeviceAddress),
             .clusterReferences = set.addresses->_device_address + VkDeviceAddress(r.first) * sizeof(VkDeviceAddress)});
        blases.push_back(std::make_shared<vulkan_blas>(_ctx, storage, VK_NULL_HANDLE, address,
                                                       isize(sizes.accelerationStructureSize), 0, 0, flags,
                                                       clusters->max_geometry_index() + 1));
        storages.push_back(storage);
    }

    auto const all = input_of(total, max_per);
    auto const input = info_of(&all, u32(ranges.size()));
    auto sizes = VkAccelerationStructureBuildSizesInfoKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt.get_cluster_build_sizes(_ctx._device, &input, &sizes);

    // One transient buffer: the destinations, then the records, then their count.
    auto const record_size = isize(sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV));
    auto const records_at = isize(aligned(VkDeviceAddress(destinations.size() * isize(sizeof(VkDeviceAddress))), 16));
    auto const count_at = isize(aligned(VkDeviceAddress(records_at + records.size() * record_size), 16));
    auto bytes = cc::vector<byte>::create_filled(count_at + isize(sizeof(u32)), byte(0));
    cc::memcpy(bytes.data(), destinations.data(), size_t(destinations.size() * isize(sizeof(VkDeviceAddress))));
    cc::memcpy(bytes.data() + records_at, records.data(), size_t(records.size() * record_size));
    auto const count = u32(records.size());
    cc::memcpy(bytes.data() + count_at, &count, sizeof(count));
    // the build reads the destinations as a storage buffer
    auto const args_raw = _ctx.transient.create_raw_buffer(bytes.size(), sg::buffer_usage::accel_structure_build_input
                                                                             | sg::buffer_usage::readwrite_buffer
                                                                             | sg::buffer_usage::copy_dst);
    auto const args = vulkan_buffer_of(args_raw, "build_cluster_blas: the records");
    upload_bytes_to_buffer(args_raw, cc::span<byte const>(bytes), 0);

    auto const scratch = vulkan_buffer_of(
        _ctx.transient.create_raw_buffer(isize(sizes.buildScratchSize) + isize(props.clusterScratchByteAlignment),
                                         sg::buffer_usage::readwrite_buffer),
        "build_cluster_blas: the scratch");

    // The clusters and their addresses are read; each blas is written.
    track_buffer_access(*set.storage, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_read);
    track_buffer_access(*set.addresses, sg::pipeline_stage_flag::accel_build,
                        sg::access_flag::accel_read | sg::access_flag::shader_read);
    for (auto const& s : storages)
        track_buffer_access(*s, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    track_buffer_access(*scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(*args, sg::pipeline_stage_flag::accel_build,
                        sg::access_flag::indirect_read | sg::access_flag::shader_read);
    flush_barriers();

    auto const commands = VkClusterAccelerationStructureCommandsInfoNV{
        .sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV,
        .input = input,
        .dstImplicitData = 0,
        .scratchData = aligned(scratch->_device_address, props.clusterScratchByteAlignment),
        .dstAddressesArray = {.deviceAddress = args->_device_address,
                              .stride = sizeof(VkDeviceAddress),
                              .size = VkDeviceSize(destinations.size() * isize(sizeof(VkDeviceAddress)))},
        .dstSizesArray = {},
        .srcInfosArray = {.deviceAddress = args->_device_address + VkDeviceAddress(records_at),
                          .stride = VkDeviceSize(record_size),
                          .size = VkDeviceSize(records.size() * record_size)},
        .srcInfosCount = args->_device_address + VkDeviceAddress(count_at),
        .addressResolutionFlags = 0,
    };
    rt.cmd_build_clusters(_buffer, &commands);

    keep_bound(clusters);
    return blases;
}
} // namespace sg::backend::vulkan
