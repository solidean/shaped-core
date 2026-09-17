// metal_raytracing: ray-tracing acceleration-structure builds (cmd.raytracing).
// Translates the backend-neutral geometry / instance inputs to MTL4 descriptors, sizes the result from the device, and
// records the build on the compute encoder — which is where MTL4 puts acceleration-structure work, alongside dispatch
// and the copies that stand in for a blit encoder.
// See libs/graphics/shaped-graphics/docs/concepts/acceleration-structures.md.

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/metal/metal_acceleration_structure.hh>
#include <shaped-graphics/backends/metal/metal_command_list.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
namespace
{
[[nodiscard]] MTL::AccelerationStructureUsage to_metal_usage(sg::accel_build_flags f)
{
    CC_ASSERT(!f.has_all(sg::accel_build_flag::fast_trace | sg::accel_build_flag::fast_build),
              "fast_trace and fast_build are mutually exclusive");

    auto out = MTL::AccelerationStructureUsage(MTL::AccelerationStructureUsageNone);
    if (f.has(sg::accel_build_flag::fast_trace))
        out = MTL::AccelerationStructureUsage(out | MTL::AccelerationStructureUsagePreferFastIntersection);
    if (f.has(sg::accel_build_flag::fast_build))
        out = MTL::AccelerationStructureUsage(out | MTL::AccelerationStructureUsagePreferFastBuild);
    if (f.has(sg::accel_build_flag::allow_update))
        out = MTL::AccelerationStructureUsage(out | MTL::AccelerationStructureUsageRefit);
    if (f.has(sg::accel_build_flag::minimize_memory))
        out = MTL::AccelerationStructureUsage(out | MTL::AccelerationStructureUsageMinimizeMemory);

    // accel_build_flag::allow_compaction has no counterpart and needs none: Metal's
    // copyAndCompactAccelerationStructure works on any built structure, where DXR and Vulkan want the opt-in at build.
    // Accepted and ignored, which is Metal asking for less ceremony rather than offering less capability.
    return out;
}

/// Downcast and validate a build-input buffer, asserting the contract the sg scope forwards without checking.
[[nodiscard]] metal_buffer const& require_build_input(sg::raw_buffer_handle const& buffer, char const* what)
{
    CC_ASSERT(buffer != nullptr, "acceleration-structure build input buffer is null");
    CC_ASSERT(!buffer->is_expired(), "build input buffer is a transient buffer used past its epoch (expired)");
    CC_ASSERT(buffer->usage().has(sg::buffer_usage::accel_structure_build_input),
              "acceleration-structure build input buffer must have buffer_usage::accel_structure_build_input");
    (void)what;
    return static_cast<metal_buffer const&>(*buffer);
}

[[nodiscard]] MTL4::BufferRange range_of(metal_buffer const& buffer, isize offset_in_bytes, isize length_in_bytes)
{
    CC_ASSERT(offset_in_bytes >= 0 && length_in_bytes >= 0, "a build input range must not be negative");
    CC_ASSERT(offset_in_bytes + length_in_bytes <= buffer.size_in_bytes(), "a build input range is out of bounds");
    return MTL4::BufferRange::Make(buffer.gpu_address() + u64(offset_in_bytes), u64(length_in_bytes));
}
} // namespace

bool metal_command_list::raytracing_is_supported() const
{
    return _metal_context.supports(sg::feature::raytracing);
}

MTL::AccelerationStructure* metal_command_list::build_accel_common(MTL4::AccelerationStructureDescriptor* descriptor,
                                                                   isize& out_size,
                                                                   isize& out_build_scratch,
                                                                   isize& out_update_scratch)
{
    auto* const device = _metal_context.device();

    // One query answers all three sizes, refit scratch included — which is the number update_scratch_size_in_bytes
    // holds a slot for, long before anything refits.
    auto const sizes = device->accelerationStructureSizes(descriptor);
    CC_ASSERT(sizes.accelerationStructureSize > 0, "metal reported a zero-size acceleration structure");

    out_size = isize(sizes.accelerationStructureSize);
    out_build_scratch = isize(sizes.buildScratchBufferSize);
    out_update_scratch = isize(sizes.refitScratchBufferSize);

    auto* const accel = device->newAccelerationStructure(NS::UInteger(sizes.accelerationStructureSize));
    CC_ASSERT(accel != nullptr, "the metal device refused an acceleration structure");

    // Residency is declared rather than inferred: MTL4 removed useResource, and a structure outside every residency
    // set the queue knows about is simply not there when the build runs — with no validation message to say so.
    _metal_context.residency().add(accel);
    return accel;
}

sg::blas_handle metal_command_list::build_blas_common(MTL4::PrimitiveAccelerationStructureDescriptor* descriptor,
                                                      cc::span<sg::raw_buffer_handle const> input_buffers,
                                                      sg::accel_build_flags flags,
                                                      int geometry_count)
{
    CC_ASSERT(raytracing_is_supported(), "ray tracing is not supported on this device (check "
                                         "cmd.raytracing.is_supported())");

    auto size = isize(0);
    auto build_scratch = isize(0);
    auto update_scratch = isize(0);
    auto* const accel = build_accel_common(descriptor, size, build_scratch, update_scratch);

    auto const result = std::make_shared<metal_blas>(_metal_context, accel, size, build_scratch, update_scratch, flags,
                                                     geometry_count);

    auto const scratch_raw
        = _metal_context.transient.create_raw_buffer(build_scratch, sg::buffer_usage::readwrite_buffer);

    // Only the structure itself is an acceleration structure (accel_write).
    // Scratch is a plain read-write buffer and the geometry inputs are ordinary reads, so a prior upload of them
    // becomes the copy-to-read barrier through the same tracker every other resource uses.
    declare_accel(result, result->storage(), sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    declare_buffer(scratch_raw, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    for (auto const& in : input_buffers)
        declare_buffer(in, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    flush_barriers();

    auto const& scratch = static_cast<metal_buffer const&>(*scratch_raw);
    compute_encoder()->buildAccelerationStructure(accel, descriptor, range_of(scratch, 0, build_scratch));

    return result;
}

sg::blas_handle metal_command_list::raytracing_build_blas_triangles(cc::span<blas_triangles const> geometries,
                                                                    accel_build_flags flags)
{
    CC_ASSERT(!geometries.empty(), "build_blas needs at least one geometry");
    auto const scope = autorelease_scope();

    cc::vector<sg::raw_buffer_handle> inputs;
    cc::vector<MTL4::AccelerationStructureTriangleGeometryDescriptor*> geometry_descs;

    for (auto const& g : geometries)
    {
        CC_ASSERT(g.vertex_count > 0, "triangle geometry needs a positive vertex_count");
        auto const& vertices = require_build_input(g.vertices, "vertices");

        auto const triangle_count = g.indices != nullptr ? g.index_count / 3 : g.vertex_count / 3;
        if (g.indices != nullptr)
            CC_ASSERT(g.index_count % 3 == 0, "an indexed triangle geometry needs index_count % 3 == 0");
        else
            CC_ASSERT(g.vertex_count % 3 == 0, "a non-indexed triangle geometry needs vertex_count % 3 == 0");
        CC_ASSERT(triangle_count > 0, "triangle geometry resolves to zero triangles");

        auto* const d = MTL4::AccelerationStructureTriangleGeometryDescriptor::alloc()->init();
        d->setVertexBuffer(range_of(vertices, g.vertex_offset_in_bytes, g.vertex_count * g.vertex_stride_in_bytes));
        d->setVertexStride(NS::UInteger(g.vertex_stride_in_bytes));
        d->setVertexFormat(MTL::AttributeFormatFloat3);
        d->setTriangleCount(NS::UInteger(triangle_count));
        d->setOpaque(g.is_opaque);

        // **DXR's geometry contribution to the hit index, which Metal spells per geometry descriptor.**
        // The instance's own offset alone makes every geometry of a BLAS select one hit group, so a BLAS whose second
        // geometry needs a different any-hit would run the first's.
        // The multiplier is 1 here, which is what sg's surface implies — see
        // libs/graphics/shaped-graphics/docs/concepts/raytracing-pipeline.md for the contribution metal has no
        // counterpart for.
        d->setIntersectionFunctionTableOffset(NS::UInteger(geometry_descs.size()));

        if (g.indices != nullptr)
        {
            auto const& indices = require_build_input(g.indices, "indices");
            auto const index_width = g.index_type == sg::index_format::uint16 ? isize(2) : isize(4);
            d->setIndexBuffer(range_of(indices, g.index_offset_in_bytes, g.index_count * index_width));
            d->setIndexType(g.index_type == sg::index_format::uint16 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32);
            inputs.push_back(g.indices);
        }

        if (g.transform != nullptr)
        {
            auto const& transform = require_build_input(g.transform, "transform");
            d->setTransformationMatrixBuffer(range_of(transform, g.transform_offset_in_bytes, 12 * isize(sizeof(float))));
            // sg's transform is row-major 3x4 — the DXR / Vulkan wire layout — and Metal takes it verbatim rather than
            // transposed, which is what MatrixLayout is for.
            d->setTransformationMatrixLayout(MTL::MatrixLayoutRowMajor);
            inputs.push_back(g.transform);
        }

        inputs.push_back(g.vertices);
        geometry_descs.push_back(d);
    }

    auto* const array = NS::Array::array(reinterpret_cast<NS::Object* const*>(geometry_descs.data()),
                                         NS::UInteger(geometry_descs.size()));

    auto* const descriptor = MTL4::PrimitiveAccelerationStructureDescriptor::alloc()->init();
    descriptor->setGeometryDescriptors(array);
    descriptor->setUsage(to_metal_usage(flags));

    auto const result = build_blas_common(descriptor, inputs, flags, int(geometries.size()));

    descriptor->release();
    for (auto* const d : geometry_descs)
        d->release();
    return result;
}

sg::blas_handle metal_command_list::raytracing_build_blas_aabbs(cc::span<blas_aabbs const> geometries,
                                                                accel_build_flags flags)
{
    CC_ASSERT(!geometries.empty(), "build_blas needs at least one geometry");
    auto const scope = autorelease_scope();

    cc::vector<sg::raw_buffer_handle> inputs;
    cc::vector<MTL4::AccelerationStructureBoundingBoxGeometryDescriptor*> geometry_descs;

    for (auto const& g : geometries)
    {
        CC_ASSERT(g.aabb_count > 0, "procedural geometry needs a positive aabb_count");
        CC_ASSERT(g.aabb_stride_in_bytes % 8 == 0, "aabb_stride_in_bytes must be a multiple of 8");
        auto const& aabbs = require_build_input(g.aabbs, "aabbs");

        auto* const d = MTL4::AccelerationStructureBoundingBoxGeometryDescriptor::alloc()->init();
        d->setBoundingBoxBuffer(range_of(aabbs, g.aabb_offset_in_bytes, g.aabb_count * g.aabb_stride_in_bytes));
        d->setBoundingBoxStride(NS::UInteger(g.aabb_stride_in_bytes));
        d->setBoundingBoxCount(NS::UInteger(g.aabb_count));
        d->setOpaque(g.is_opaque);
        d->setIntersectionFunctionTableOffset(NS::UInteger(geometry_descs.size())); // see the triangle path

        inputs.push_back(g.aabbs);
        geometry_descs.push_back(d);
    }

    auto* const array = NS::Array::array(reinterpret_cast<NS::Object* const*>(geometry_descs.data()),
                                         NS::UInteger(geometry_descs.size()));

    auto* const descriptor = MTL4::PrimitiveAccelerationStructureDescriptor::alloc()->init();
    descriptor->setGeometryDescriptors(array);
    descriptor->setUsage(to_metal_usage(flags));

    auto const result = build_blas_common(descriptor, inputs, flags, int(geometries.size()));

    descriptor->release();
    for (auto* const d : geometry_descs)
        d->release();
    return result;
}

sg::tlas_handle metal_command_list::raytracing_build_tlas(cc::span<tlas_instance const> instances, accel_build_flags flags)
{
    CC_ASSERT(raytracing_is_supported(), "ray tracing is not supported on this device (check "
                                         "cmd.raytracing.is_supported())");
    CC_ASSERT(!instances.empty(), "build_tlas needs at least one instance");
    auto const scope = autorelease_scope();

    // **MTL4 requires the descriptor Metal calls "indirect".**
    // `setInstancedAccelerationStructures` — the side array the default and userID descriptors index into — exists
    // only on the Metal 3 descriptor, so an MTL4 build names each BLAS by `accelerationStructureID` instead.
    // Despite the name that is the direct analogue of DXR's by-address reference, and its fields are exactly
    // sg::tlas_instance's.
    cc::vector<MTL::IndirectAccelerationStructureInstanceDescriptor> instance_descs;
    instance_descs.reserve(instances.size());
    cc::vector<sg::blas_handle> referenced_blases;

    for (auto const& inst : instances)
    {
        CC_ASSERT(inst.blas != nullptr, "tlas_instance.blas is null");
        CC_ASSERT(!inst.blas->is_expired(), "tlas_instance.blas is expired");
        CC_ASSERT(inst.instance_id < (1u << 24), "tlas_instance.instance_id must fit in 24 bits");
        CC_ASSERT(inst.hit_group_offset < (1u << 24), "tlas_instance.hit_group_offset must fit in 24 bits");
        auto const& mtl_blas = static_cast<metal_blas const&>(*inst.blas);

        MTL::IndirectAccelerationStructureInstanceDescriptor d = {};
        // sg's transform is row-major 3x4 and PackedFloat4x3 is four packed float3 columns, so the two are transposes.
        // The descriptor's layout is fixed, unlike the geometry transform's, so this one is transposed by hand.
        for (int col = 0; col < 4; ++col)
        {
            d.transformationMatrix.columns[col].x = inst.transform[0 * 4 + col];
            d.transformationMatrix.columns[col].y = inst.transform[1 * 4 + col];
            d.transformationMatrix.columns[col].z = inst.transform[2 * 4 + col];
        }
        d.userID = inst.instance_id;
        d.mask = inst.mask;
        d.intersectionFunctionTableOffset = inst.hit_group_offset;
        d.accelerationStructureID = mtl_blas.storage().resource_id();

        auto options = MTL::AccelerationStructureInstanceOptions(MTL::AccelerationStructureInstanceOptionNone);
        switch (inst.cull_mode)
        {
        case sg::instance_cull_mode::back:
            break; // default winding, no flag
        case sg::instance_cull_mode::front:
            options = MTL::AccelerationStructureInstanceOptions(
                options | MTL::AccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise);
            break;
        case sg::instance_cull_mode::none:
            options = MTL::AccelerationStructureInstanceOptions(
                options | MTL::AccelerationStructureInstanceOptionDisableTriangleCulling);
            break;
        }
        if (inst.opaque_override.has_value())
            options = MTL::AccelerationStructureInstanceOptions(
                options
                | (inst.opaque_override.value() ? MTL::AccelerationStructureInstanceOptionOpaque
                                                : MTL::AccelerationStructureInstanceOptionNonOpaque));
        d.options = options;

        instance_descs.push_back(d);
        referenced_blases.push_back(inst.blas);
    }

    auto const stride = isize(sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor));
    auto const instance_raw = _metal_context.transient.create_raw_buffer(
        isize(instances.size()) * stride, sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);

    upload_bytes_to_buffer(
        instance_raw,
        cc::as_bytes(cc::span<MTL::IndirectAccelerationStructureInstanceDescriptor const>(instance_descs)), 0);

    auto const& instance_buffer = static_cast<metal_buffer const&>(*instance_raw);

    auto* const descriptor = MTL4::InstanceAccelerationStructureDescriptor::alloc()->init();
    descriptor->setInstanceDescriptorBuffer(range_of(instance_buffer, 0, isize(instances.size()) * stride));
    descriptor->setInstanceDescriptorStride(NS::UInteger(stride));
    descriptor->setInstanceCount(NS::UInteger(instances.size()));
    descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeIndirect);
    descriptor->setInstanceTransformationMatrixLayout(MTL::MatrixLayoutColumnMajor);
    descriptor->setUsage(to_metal_usage(flags));

    auto size = isize(0);
    auto build_scratch = isize(0);
    auto update_scratch = isize(0);
    auto* const accel = build_accel_common(descriptor, size, build_scratch, update_scratch);

    auto const result = std::make_shared<metal_tlas>(_metal_context, accel, size, build_scratch, update_scratch, flags,
                                                     int(instances.size()), cc::move(referenced_blases));

    auto const scratch_raw
        = _metal_context.transient.create_raw_buffer(build_scratch, sg::buffer_usage::readwrite_buffer);

    // The top-level build writes the structure, uses scratch as a plain read-write buffer, reads the packed instance
    // descriptors as an ordinary buffer, and reads every referenced BLAS *as* an acceleration structure.
    declare_accel(result, result->storage(), sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    declare_buffer(scratch_raw, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    declare_buffer(instance_raw, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    for (auto const& inst : instances)
    {
        auto const& mtl_blas = static_cast<metal_blas const&>(*inst.blas);
        declare_accel(inst.blas, mtl_blas.storage(), sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_read);
    }
    flush_barriers();

    auto const& scratch = static_cast<metal_buffer const&>(*scratch_raw);
    compute_encoder()->buildAccelerationStructure(accel, descriptor, range_of(scratch, 0, build_scratch));

    descriptor->release();
    return result;
}
} // namespace sg::backend::metal
