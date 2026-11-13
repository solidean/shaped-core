// dx12 clustered geometry: the preview runtime's ExecuteIndirectRTASOperations behind cmd.raytracing.build_clusters,
// build_clusters_indirect and build_cluster_blas.
// Every preview name stays behind SG_HAS_AGILITY_PREVIEW, since the spec still renames fields between previews.
// See libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_acceleration_structure.hh>
#include <shaped-graphics/backends/dx12/dx12_barrier.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/context/impl/device_lifecycle.hh>
#include <shaped-graphics/raytracing/blas_table.hh>
#include <shaped-graphics/raytracing/clusters.hh>

#ifndef SG_HAS_AGILITY_PREVIEW
#error "SG_HAS_AGILITY_PREVIEW must be defined by the dx12 backend's CMake"
#endif

namespace sg::backend::dx12
{
#if SG_HAS_AGILITY_PREVIEW
namespace
{
/// What every cluster-level structure's address is a multiple of (D3D12_RAYTRACING_CLAS_BYTE_ALIGNMENT), and a
/// cluster BLAS's and every scratch's.
constexpr isize k_clas_alignment = 128;
constexpr isize k_structure_alignment = 256;
/// D3D12's opaque bit of `BaseGeometryIndexAndFlags`, `D3D12_RTAS_CLUSTERED_GEOMETRY_FLAG_OPAQUE`.
constexpr UINT k_geometry_opaque = 0x8000'0000u;

[[nodiscard]] D3D12_RTAS_OPERATION_FLAGS flags_of(sg::accel_build_flags flags)
{
    auto out = D3D12_RTAS_OPERATION_FLAG_NONE;
    if (flags.has(sg::accel_build_flag::fast_trace))
        out |= D3D12_RTAS_OPERATION_FLAG_FAST_TRACE;
    if (flags.has(sg::accel_build_flag::fast_build))
        out |= D3D12_RTAS_OPERATION_FLAG_FAST_OPERATION;
    return out;
}

[[nodiscard]] D3D12_INDEX_FORMAT index_format_of(sg::index_format f)
{
    return f == sg::index_format::uint16 ? D3D12_INDEX_FORMAT_UINT16 : D3D12_INDEX_FORMAT_UINT32;
}

[[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS aligned(D3D12_GPU_VIRTUAL_ADDRESS at, isize alignment)
{
    return (at + D3D12_GPU_VIRTUAL_ADDRESS(alignment) - 1) / D3D12_GPU_VIRTUAL_ADDRESS(alignment)
         * D3D12_GPU_VIRTUAL_ADDRESS(alignment);
}

[[nodiscard]] dx12_buffer_handle dx12_buffer_of(sg::raw_buffer_handle const& buffer, char const* what)
{
    auto dx = std::dynamic_pointer_cast<dx12_buffer const>(buffer);
    CC_ASSERTF(dx != nullptr, "{} is not a dx12 buffer", what);
    return dx;
}

/// The triangle limits a build of `count` clusters states, the pool's or the described clusters' own.
[[nodiscard]] D3D12_RTAS_CLUSTER_TRIANGLES_INPUTS_DESC triangles_input_of(int count,
                                                                          int max_geometry_index,
                                                                          int max_triangles,
                                                                          int max_vertices,
                                                                          isize total_triangles,
                                                                          isize total_vertices,
                                                                          sg::index_format index_type,
                                                                          sg::accel_build_flags flags)
{
    return {
        .ClusterLimits = {.MaxArgCount = UINT(count),
                          .MaxGeometryIndexValue = UINT(max_geometry_index),
                          .MaxUniqueGeometryIndexAndFlagsCountPerCluster = 1,
                          .MaxTriangleCountPerCluster = UINT(max_triangles),
                          .MaxUniqueVertexIndexCountPerCluster = UINT(max_vertices),
                          .MaxTotalTriangleCount = UINT(total_triangles),
                          .MaxSumOfUniqueVertexIndexCounts = UINT(total_vertices),
                          .MaxOpacityMicromapIndicesPerCluster = 0},
        .Flags = flags_of(flags),
        .Mode = D3D12_RTAS_OPERATION_MODE_IMPLICIT_DESTINATIONS,
        .VertexFormat = D3D12_VERTEX_FORMAT_FLOAT32_3,
        .IndexFormat = index_format_of(index_type),
        .GeometryIndexAndFlagsIndexFormat = D3D12_INDEX_FORMAT_NONE,
        .OpacityMicromapIndexFormat = D3D12_INDEX_FORMAT_NONE,
    };
}

[[nodiscard]] ComPtr<ID3D12DeviceRaytracing2> raytracing2_of(ComPtr<ID3D12Device> const& device)
{
    ComPtr<ID3D12DeviceRaytracing2> out;
    if (FAILED(device.As(&out)))
        return nullptr;
    return out;
}
} // namespace

cc::result<sg::cluster_pool_handle> dx12_context::try_create_cluster_pool(sg::cluster_pool_description const& desc)
{
    if (!_clusters_and_ptlas)
        return cc::error("create_cluster_pool: this device reports no ClustersAndPTLASSupported");
    auto const device = raytracing2_of(_device);
    if (device == nullptr)
        return cc::error("create_cluster_pool: the runtime has no ID3D12DeviceRaytracing2");
    auto const triangles = triangles_input_of(desc.max_clusters, desc.max_geometry_index, desc.max_triangles_per_cluster,
                                              desc.max_vertices_per_cluster, desc.max_total_triangles,
                                              desc.max_total_vertices, desc.index_type, desc.flags);
    auto const inputs = D3D12_RTAS_OPERATION_INPUTS{.Type = D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES,
                                                    .pClusterTrianglesDesc = &triangles};
    auto prebuild = D3D12_RTAS_OPERATION_PREBUILD_INFO{};
    {
        sg::impl::raytracing_driver_hold const driver_guard;
        device->GetRTASOperationPrebuildInfo(&inputs, &prebuild);
    }
    if (prebuild.ResultDataMaxSizeInBytes == 0)
        return cc::error("create_cluster_pool: the device sized the pool at zero bytes");
    // a committed buffer starts 64 KiB aligned, past the 128 a cluster needs
    auto storage = dx12_buffer_of(persistent.create_raw_buffer(isize(prebuild.ResultDataMaxSizeInBytes),
                                                               sg::buffer_usage::accel_structure_storage),
                                  "create_cluster_pool: the storage");
    auto addresses = dx12_buffer_of(
        persistent.create_raw_buffer(isize(desc.max_clusters) * isize(sizeof(D3D12_GPU_VIRTUAL_ADDRESS)),
                                     sg::buffer_usage::readwrite_buffer | sg::buffer_usage::accel_structure_build_input),
        "create_cluster_pool: the address array");
    return sg::cluster_pool_handle(std::make_shared<dx12_cluster_pool>(desc, isize(prebuild.ResultDataMaxSizeInBytes),
                                                                       isize(prebuild.ScratchDataSizeInBytes),
                                                                       cc::move(storage), cc::move(addresses)));
}

sg::cluster_set_handle dx12_command_list::raytracing_build_clusters(cc::span<sg::cluster_triangles const> clusters,
                                                                    sg::accel_build_flags flags)
{
    ComPtr<ID3D12CommandListRaytracing2> list;
    [[maybe_unused]] HRESULT const list_hr = _list.As(&list);
    CC_ASSERT(SUCCEEDED(list_hr) && list, "build_clusters: the command list has no ID3D12CommandListRaytracing2");
    auto const device = raytracing2_of(_ctx._device);
    CC_ASSERT(device != nullptr, "build_clusters: the runtime has no ID3D12DeviceRaytracing2");

    // D3D12 states the index format per build, so one build's clusters share it
    auto const index_type = clusters[0].index_type;
    auto max_triangles = 0;
    auto max_vertices = 0;
    auto max_geometry_index = 0;
    auto total_triangles = isize(0);
    auto total_vertices = isize(0);
    for (auto const& c : clusters)
    {
        CC_ASSERT(c.index_type == index_type, "build_clusters: dx12 builds every cluster of one call with one index "
                                              "format");
        max_triangles = cc::max(max_triangles, c.triangle_count);
        max_vertices = cc::max(max_vertices, c.vertex_count);
        max_geometry_index = cc::max(max_geometry_index, c.geometry_index);
        total_triangles += c.triangle_count;
        total_vertices += c.vertex_count;
    }
    auto const triangles = triangles_input_of(int(clusters.size()), max_geometry_index, max_triangles, max_vertices,
                                              total_triangles, total_vertices, index_type, flags);
    auto const inputs = D3D12_RTAS_OPERATION_INPUTS{.Type = D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES,
                                                    .pClusterTrianglesDesc = &triangles};
    auto prebuild = D3D12_RTAS_OPERATION_PREBUILD_INFO{};
    {
        sg::impl::raytracing_driver_hold const driver_guard;
        device->GetRTASOperationPrebuildInfo(&inputs, &prebuild);
    }
    CC_ASSERT(prebuild.ResultDataMaxSizeInBytes > 0, "build_clusters: the device sized the clusters at zero bytes");

    auto const storage = dx12_buffer_of(_ctx.persistent.create_raw_buffer(isize(prebuild.ResultDataMaxSizeInBytes),
                                                                          sg::buffer_usage::accel_structure_storage),
                                        "build_clusters: the storage");
    auto const addresses
        = dx12_buffer_of(_ctx.persistent.create_raw_buffer(
                             clusters.size() * isize(sizeof(D3D12_GPU_VIRTUAL_ADDRESS)),
                             sg::buffer_usage::readwrite_buffer | sg::buffer_usage::accel_structure_build_input),
                         "build_clusters: the address array");

    // Each cluster's arguments as the preview runtime lays them out, then their count.
    auto args = cc::vector<D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS>();
    auto inputs_read = cc::vector<dx12_buffer_handle>();
    for (auto const& c : clusters)
    {
        auto const vertices = dx12_buffer_of(c.vertices, "build_clusters: a vertex buffer");
        auto const indices = dx12_buffer_of(c.indices, "build_clusters: an index buffer");
        args.push_back({
            .ClusterID = c.cluster_id,
            .ClusterFlags = 0,
            .TriangleCount = UINT16(c.triangle_count),
            .UniqueVertexIndexCount = UINT16(c.vertex_count),
            .BaseGeometryIndexAndFlags = UINT(c.geometry_index) | (c.is_opaque ? k_geometry_opaque : 0u),
            .VertexBufferStride = UINT16(c.vertex_stride_in_bytes),
            .IndexBufferStride = UINT16(index_type == sg::index_format::uint16 ? 2 : 4),
            .VertexBuffer = vertices->gpu_virtual_address() + D3D12_GPU_VIRTUAL_ADDRESS(c.vertex_offset_in_bytes),
            .IndexBuffer = indices->gpu_virtual_address() + D3D12_GPU_VIRTUAL_ADDRESS(c.index_offset_in_bytes),
        });
        inputs_read.push_back(vertices);
        inputs_read.push_back(indices);
    }
    auto const arg_size = isize(sizeof(D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS));
    auto const count_at = (isize(args.size()) * arg_size + 15) / 16 * 16;
    auto bytes = cc::vector<byte>::create_filled(count_at + isize(sizeof(UINT)), byte(0));
    cc::memcpy(bytes.data(), args.data(), size_t(isize(args.size()) * arg_size));
    auto const count = UINT(args.size());
    cc::memcpy(bytes.data() + count_at, &count, sizeof(count));
    auto const args_raw = _ctx.transient.create_raw_buffer(
        bytes.size(), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto const args_buffer = dx12_buffer_of(args_raw, "build_clusters: the arguments");
    upload_bytes_to_buffer(args_raw, cc::span<byte const>(bytes), 0);

    auto const scratch
        = dx12_buffer_of(_ctx.transient.create_raw_buffer(isize(prebuild.ScratchDataSizeInBytes) + k_structure_alignment,
                                                          sg::buffer_usage::readwrite_buffer),
                         "build_clusters: the scratch");

    // The clusters are written, their addresses with them as a UAV; the arguments and the inputs are shader reads.
    track_buffer_access(storage, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    track_buffer_access(addresses, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(args_buffer, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    for (auto const& in : inputs_read)
        track_buffer_access(in, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    flush_barriers();

    auto const data = D3D12_RTAS_BATCHED_OPERATION_DATA{
        .AddressResolutionFlags = D3D12_RTAS_OPERATION_ADDRESS_RESOLUTION_FLAG_NONE,
        .BatchResultData = aligned(storage->gpu_virtual_address(), k_clas_alignment),
        .BatchScratchData = aligned(scratch->gpu_virtual_address(), k_structure_alignment),
        .ResultAddressArray
        = {.StartAddress = addresses->gpu_virtual_address(), .StrideInBytes = sizeof(D3D12_GPU_VIRTUAL_ADDRESS)},
        .ResultSizeArray = {},
        .IndirectArgumentArray = {.StartAddress = args_buffer->gpu_virtual_address(), .StrideInBytes = UINT64(arg_size)},
        .IndirectArgumentArraySize = args_buffer->gpu_virtual_address() + D3D12_GPU_VIRTUAL_ADDRESS(count_at),
        .ToolsInfo = 0,
    };
    auto const desc = D3D12_RTAS_OPERATION_DESC{.Inputs = inputs, .pBatchedOperationData = &data};
    {
        sg::impl::raytracing_driver_hold const driver_guard;
        list->ExecuteIndirectRTASOperations(1, &desc, D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
    }

    for (auto& in : inputs_read)
        keep_bound(cc::move(in));
    return std::make_shared<dx12_cluster_set>(int(clusters.size()), max_geometry_index,
                                              isize(prebuild.ResultDataMaxSizeInBytes), storage, addresses);
}

void dx12_command_list::raytracing_build_clusters_indirect(sg::cluster_pool const& pool,
                                                           sg::cluster_indirect_build const& build)
{
    ComPtr<ID3D12CommandListRaytracing2> list;
    [[maybe_unused]] HRESULT const list_hr = _list.As(&list);
    CC_ASSERT(SUCCEEDED(list_hr) && list, "build_clusters_indirect: the command list has no "
                                          "ID3D12CommandListRaytracing2");
    auto const& dx = static_cast<dx12_cluster_pool const&>(pool);
    auto const& d = pool.description();
    auto const& r = build.clusters;
    auto const records = dx12_buffer_of(r.records, "build_clusters_indirect: the records");
    auto const count = dx12_buffer_of(r.count, "build_clusters_indirect: the count");

    auto const scratch
        = dx12_buffer_of(_ctx.transient.create_raw_buffer(dx._scratch_size_in_bytes + k_structure_alignment,
                                                          sg::buffer_usage::readwrite_buffer),
                         "build_clusters_indirect: the scratch");

    track_buffer_access(dx._dx12_storage, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    track_buffer_access(dx._addresses, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(records, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    if (count != records)
        track_buffer_access(count, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    for (auto const& table : build.tables)
        for (auto const& b : table->buffers())
            track_buffer_access(dx12_buffer_of(b, "build_clusters_indirect: a table's buffer"),
                                sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    flush_barriers();
    auto after_traces = make_empty_global_barrier();
    merge_into_global_barrier(after_traces, make_buffer_barrier(nullptr, sg::impl::pool_rebuild_after_traces()));
    auto const globals = cc::span<D3D12_GLOBAL_BARRIER const>(&after_traces, 1);
    count_barriers(_stats, cc::span<D3D12_BUFFER_BARRIER const>(), cc::span<D3D12_TEXTURE_BARRIER const>(), globals);
    submit_barriers(_list.Get(), {}, {}, globals);

    auto const triangles = triangles_input_of(d.max_clusters, d.max_geometry_index, d.max_triangles_per_cluster,
                                              d.max_vertices_per_cluster, d.max_total_triangles, d.max_total_vertices,
                                              d.index_type, d.flags);
    auto const data = D3D12_RTAS_BATCHED_OPERATION_DATA{
        .AddressResolutionFlags = D3D12_RTAS_OPERATION_ADDRESS_RESOLUTION_FLAG_NONE,
        .BatchResultData = aligned(dx._dx12_storage->gpu_virtual_address(), k_clas_alignment),
        .BatchScratchData = aligned(scratch->gpu_virtual_address(), k_structure_alignment),
        .ResultAddressArray
        = {.StartAddress = dx._addresses->gpu_virtual_address(), .StrideInBytes = sizeof(D3D12_GPU_VIRTUAL_ADDRESS)},
        .ResultSizeArray = {},
        .IndirectArgumentArray
        = {.StartAddress = records->gpu_virtual_address() + D3D12_GPU_VIRTUAL_ADDRESS(r.offset_in_bytes),
           .StrideInBytes = sizeof(D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS)},
        .IndirectArgumentArraySize = count->gpu_virtual_address() + D3D12_GPU_VIRTUAL_ADDRESS(r.count_offset_in_bytes),
        .ToolsInfo = 0,
    };
    auto const desc = D3D12_RTAS_OPERATION_DESC{
        .Inputs = {.Type = D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES, .pClusterTrianglesDesc = &triangles},
        .pBatchedOperationData = &data};
    {
        sg::impl::raytracing_driver_hold const driver_guard;
        list->ExecuteIndirectRTASOperations(1, &desc, D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
    }

    keep_bound(r.records);
    keep_bound(r.count);
    for (auto const& table : build.tables)
        keep_bound(table);
    keep_bound(pool.shared_from_this());
}

cc::vector<sg::blas_handle> dx12_command_list::raytracing_build_cluster_blas(sg::cluster_set_handle const& clusters,
                                                                             cc::span<sg::cluster_range const> ranges,
                                                                             sg::accel_build_flags flags,
                                                                             int /*hit_record_stride*/)
{
    ComPtr<ID3D12CommandListRaytracing2> list;
    [[maybe_unused]] HRESULT const list_hr = _list.As(&list);
    CC_ASSERT(SUCCEEDED(list_hr) && list, "build_cluster_blas: the command list has no ID3D12CommandListRaytracing2");
    auto const device = raytracing2_of(_ctx._device);
    CC_ASSERT(device != nullptr, "build_cluster_blas: the runtime has no ID3D12DeviceRaytracing2");

    // the storage and the addresses of a set or a pool
    auto storage = dx12_buffer_handle();
    auto addresses = dx12_buffer_handle();
    if (auto const* const set = dynamic_cast<dx12_cluster_set const*>(clusters.get()))
    {
        storage = set->_dx12_storage;
        addresses = set->_addresses;
    }
    else
    {
        auto const* const pool = dynamic_cast<dx12_cluster_pool const*>(clusters.get());
        CC_ASSERT(pool != nullptr, "build_cluster_blas: the clusters are no dx12 cluster set or pool");
        storage = pool->_dx12_storage;
        addresses = pool->_addresses;
    }

    auto const input_of = [&](UINT count, UINT total, UINT max_per)
    {
        return D3D12_RTAS_CLAS_INPUTS_DESC{.Flags = flags_of(flags),
                                           .MaxArgCount = count,
                                           .Mode = D3D12_RTAS_OPERATION_MODE_EXPLICIT_DESTINATIONS,
                                           .MaxTotalClasCount = total,
                                           .MaxClasCountPerArg = max_per};
    };
    auto const prebuild_of = [&](D3D12_RTAS_CLAS_INPUTS_DESC const& in)
    {
        auto const inputs
            = D3D12_RTAS_OPERATION_INPUTS{.Type = D3D12_RTAS_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS, .pClasDesc = &in};
        auto prebuild = D3D12_RTAS_OPERATION_PREBUILD_INFO{};
        sg::impl::raytracing_driver_hold const driver_guard;
        device->GetRTASOperationPrebuildInfo(&inputs, &prebuild);
        return prebuild;
    };

    // Each BLAS into storage of its own: explicit mode sizes one result, so the CPU knows the address a tlas names.
    auto total = UINT(0);
    auto max_per = UINT(0);
    auto blases = cc::vector<sg::blas_handle>();
    auto destinations = cc::vector<D3D12_GPU_VIRTUAL_ADDRESS>();
    auto args = cc::vector<D3D12_RTAS_OPERATION_BUILD_BLAS_FROM_CLAS_ARGS>();
    auto written = cc::vector<dx12_buffer_handle>();
    for (auto const& r : ranges)
    {
        total += UINT(r.count);
        max_per = cc::max(max_per, UINT(r.count));
        auto const one = prebuild_of(input_of(1, UINT(r.count), UINT(r.count)));
        CC_ASSERT(one.ResultDataMaxSizeInBytes > 0, "build_cluster_blas: the device sized a blas at zero bytes");
        auto const blas_storage
            = dx12_buffer_of(_ctx.persistent.create_raw_buffer(isize(one.ResultDataMaxSizeInBytes),
                                                               sg::buffer_usage::accel_structure_storage),
                             "build_cluster_blas: a blas's storage");
        destinations.push_back(blas_storage->gpu_virtual_address());
        args.push_back({.ClasAddressCount = UINT(r.count),
                        .ClasAddressStride = sizeof(D3D12_GPU_VIRTUAL_ADDRESS),
                        .ClasAddressArray = addresses->gpu_virtual_address()
                                          + D3D12_GPU_VIRTUAL_ADDRESS(r.first) * sizeof(D3D12_GPU_VIRTUAL_ADDRESS)});
        blases.push_back(std::make_shared<dx12_blas>(blas_storage, isize(one.ResultDataMaxSizeInBytes), 0, 0, flags,
                                                     clusters->max_geometry_index() + 1));
        written.push_back(blas_storage);
    }
    auto const all = input_of(UINT(ranges.size()), total, max_per);
    auto const prebuild = prebuild_of(all);

    // One transient buffer: the destinations, then the arguments, then their count.
    auto const arg_size = isize(sizeof(D3D12_RTAS_OPERATION_BUILD_BLAS_FROM_CLAS_ARGS));
    auto const args_at = (destinations.size() * isize(sizeof(D3D12_GPU_VIRTUAL_ADDRESS)) + 15) / 16 * 16;
    auto const count_at = (args_at + isize(args.size()) * arg_size + 15) / 16 * 16;
    auto bytes = cc::vector<byte>::create_filled(count_at + isize(sizeof(UINT)), byte(0));
    cc::memcpy(bytes.data(), destinations.data(), size_t(destinations.size() * isize(sizeof(D3D12_GPU_VIRTUAL_ADDRESS))));
    cc::memcpy(bytes.data() + args_at, args.data(), size_t(isize(args.size()) * arg_size));
    auto const count = UINT(args.size());
    cc::memcpy(bytes.data() + count_at, &count, sizeof(count));
    auto const input_raw = _ctx.transient.create_raw_buffer(
        bytes.size(), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto const input = dx12_buffer_of(input_raw, "build_cluster_blas: the arguments");
    upload_bytes_to_buffer(input_raw, cc::span<byte const>(bytes), 0);

    auto const scratch
        = dx12_buffer_of(_ctx.transient.create_raw_buffer(isize(prebuild.ScratchDataSizeInBytes) + k_structure_alignment,
                                                          sg::buffer_usage::readwrite_buffer),
                         "build_cluster_blas: the scratch");

    // The clusters and their addresses are read; each blas is written; explicit destinations are read too.
    track_buffer_access(storage, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_read);
    track_buffer_access(addresses, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    for (auto const& s : written)
        track_buffer_access(s, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    track_buffer_access(scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    track_buffer_access(input, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    flush_barriers();

    auto const data = D3D12_RTAS_BATCHED_OPERATION_DATA{
        .AddressResolutionFlags = D3D12_RTAS_OPERATION_ADDRESS_RESOLUTION_FLAG_NONE,
        .BatchResultData = 0,
        .BatchScratchData = aligned(scratch->gpu_virtual_address(), k_structure_alignment),
        .ResultAddressArray
        = {.StartAddress = input->gpu_virtual_address(), .StrideInBytes = sizeof(D3D12_GPU_VIRTUAL_ADDRESS)},
        .ResultSizeArray = {},
        .IndirectArgumentArray = {.StartAddress = input->gpu_virtual_address() + D3D12_GPU_VIRTUAL_ADDRESS(args_at),
                                  .StrideInBytes = UINT64(arg_size)},
        .IndirectArgumentArraySize = input->gpu_virtual_address() + D3D12_GPU_VIRTUAL_ADDRESS(count_at),
        .ToolsInfo = 0,
    };
    auto const desc = D3D12_RTAS_OPERATION_DESC{
        .Inputs = {.Type = D3D12_RTAS_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS, .pClasDesc = &all},
        .pBatchedOperationData = &data};
    {
        sg::impl::raytracing_driver_hold const driver_guard;
        list->ExecuteIndirectRTASOperations(1, &desc, D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
    }

    keep_bound(clusters);
    return blases;
}
#else
cc::result<sg::cluster_pool_handle> dx12_context::try_create_cluster_pool(sg::cluster_pool_description const&)
{
    return cc::error("create_cluster_pool: dx12 builds clusters only against the preview Agility SDK "
                     "(SC_AGILITY_PREVIEW)");
}

sg::cluster_set_handle dx12_command_list::raytracing_build_clusters(cc::span<sg::cluster_triangles const>,
                                                                    sg::accel_build_flags)
{
    CC_ASSERT(false, "build_clusters: dx12 builds clusters only against the preview Agility SDK");
    return nullptr;
}

void dx12_command_list::raytracing_build_clusters_indirect(sg::cluster_pool const&, sg::cluster_indirect_build const&)
{
    CC_ASSERT(false, "build_clusters_indirect: dx12 builds clusters only against the preview Agility SDK");
}

cc::vector<sg::blas_handle> dx12_command_list::raytracing_build_cluster_blas(sg::cluster_set_handle const&,
                                                                             cc::span<sg::cluster_range const>,
                                                                             sg::accel_build_flags,
                                                                             int)
{
    CC_ASSERT(false, "build_cluster_blas: dx12 builds clusters only against the preview Agility SDK");
    return {};
}
#endif
} // namespace sg::backend::dx12
