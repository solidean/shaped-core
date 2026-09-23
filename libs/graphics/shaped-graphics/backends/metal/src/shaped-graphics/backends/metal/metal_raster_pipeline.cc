#include "metal_raster_pipeline.hh"

#include <clean-core/string/format.hh>
#include <dispatch/dispatch.h>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>
#include <shaped-graphics/backends/metal/metal_raster_state.hh>

#include <mutex>

namespace sg::backend::metal
{
namespace
{
/// One shader stage as a function descriptor Metal can build a pipeline from.
/// The library is released to the caller's care; the descriptor keeps what it needs.
struct loaded_stage
{
    MTL::Library* library = nullptr;
    MTL4::LibraryFunctionDescriptor* function = nullptr;
};

[[nodiscard]] cc::result<loaded_stage> load_stage(MTL::Device* device,
                                                  sg::compiled_shader const& shader,
                                                  cc::string_view what)
{
    if (shader.format != sg::shader_format::metal_lib)
        return cc::error(cc::format("raster_pipeline: the {} shader is not a metal library", what));
    if (shader.bytecode.empty())
        return cc::error(cc::format("raster_pipeline: the {} shader has no bytecode", what));

    auto* const blob = dispatch_data_create(shader.bytecode.data(), size_t(shader.bytecode.size()), nullptr,
                                            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NS::Error* error = nullptr;
    auto* const library = device->newLibrary(blob, &error);
    dispatch_release(blob);

    if (library == nullptr)
        return metal_error(error, cc::format("raster_pipeline: the {} library could not be loaded", what));

    auto* const function = MTL4::LibraryFunctionDescriptor::alloc()->init();
    function->setLibrary(library);
    function->setName(ns_string(shader.entry_point));

    return loaded_stage{.library = library, .function = function};
}

/// The vertex descriptor for `layout`, or null when the pipeline reads no vertex buffers at all.
///
/// **An attribute's `[[attribute(n)]]` index is its position in `attributes`**, because sg identifies a vertex input by
/// an HLSL `semantic` string and MSL has no such thing.
/// That is the same rule the vulkan backend states for its SPIR-V locations, and it is a workaround for the same
/// missing field: the backend-neutral numeric `location` on `sg::vertex_attribute` that
/// libs/graphics/shaped-graphics/docs/TODO.md already plans.
///
/// The layout index is `k_vertex_buffer_base_index + slot`, which is what `raster_bind_vertex_buffers` writes the
/// buffer's address into — the two halves have to agree or the fetch reads whatever else is bound there.
[[nodiscard]] cc::result<MTL::VertexDescriptor*> build_vertex_descriptor(sg::vertex_input_layout const& layout)
{
    if (layout.slots.empty() && layout.attributes.empty())
        return static_cast<MTL::VertexDescriptor*>(nullptr);

    if (layout.slots.size() > sg::max_vertex_buffers)
        return cc::error(cc::format("raster_pipeline: the vertex input declares {} slots, past sg's budget of {}",
                                    layout.slots.size(), sg::max_vertex_buffers));

    auto* const descriptor = MTL::VertexDescriptor::alloc()->init();

    for (auto i = isize(0); i < layout.slots.size(); ++i)
    {
        auto const& slot = layout.slots[i];

        // Metal refuses a zero stride outright, where D3D12 and Vulkan read it as "every vertex reads element 0".
        // Refused here rather than at the pipeline build, so the message names the slot.
        if (slot.stride <= 0)
        {
            descriptor->release();
            return cc::error(cc::format("raster_pipeline: vertex input slot {} has a stride of {}; metal needs a "
                                        "positive stride",
                                        i, slot.stride));
        }

        auto* const slot_layout = descriptor->layouts()->object(NS::UInteger(k_vertex_buffer_base_index + i));
        slot_layout->setStride(NS::UInteger(slot.stride));
        slot_layout->setStepFunction(slot.per_instance ? MTL::VertexStepFunctionPerInstance
                                                       : MTL::VertexStepFunctionPerVertex);
        slot_layout->setStepRate(1);
    }

    for (auto i = isize(0); i < layout.attributes.size(); ++i)
    {
        auto const& attribute = layout.attributes[i];
        if (attribute.slot < 0 || attribute.slot >= layout.slots.size())
        {
            descriptor->release();
            return cc::error(cc::format("raster_pipeline: vertex attribute '{}' names slot {}, which the layout does "
                                        "not declare",
                                        attribute.semantic, attribute.slot));
        }

        auto* const attr = descriptor->attributes()->object(NS::UInteger(i));
        attr->setFormat(vertex_format_of(attribute.format));
        attr->setOffset(NS::UInteger(attribute.offset));
        attr->setBufferIndex(NS::UInteger(k_vertex_buffer_base_index + attribute.slot));
    }

    return descriptor;
}
} // namespace

void metal_raster_pipeline::release_backend_objects()
{
    if (_state == nullptr && _depth_stencil == nullptr)
        return;

    auto* const state = _state;
    auto* const depth_stencil = _depth_stencil;
    _state = nullptr;
    _depth_stencil = nullptr;

    _ctx.epochs().defer(
        [state, depth_stencil]
        {
            if (state != nullptr)
                state->release();
            if (depth_stencil != nullptr)
                depth_stencil->release();
        });
}

cc::result<metal_raster_pipeline_handle> metal_context::create_metal_raster_pipeline(
    sg::raster_pipeline_description const& desc,
    sg::lifetime_scope)
{
    // Refused rather than ignored: a pipeline built without the stage a caller asked for draws something plausible
    // and wrong, and there is no later point at which the omission surfaces.
    if (desc.geometry_shader.has_value())
        return cc::error("raster_pipeline: the metal backend has no geometry stage");
    if (desc.tessellation_control_shader.has_value() || desc.tessellation_evaluation_shader.has_value())
        return cc::error("raster_pipeline: the metal backend has no tessellation stages");

    auto const scope = autorelease_scope();

    auto vertex = load_stage(_device, desc.vertex_shader, "vertex");
    if (vertex.has_error())
        return cc::error(vertex.error().to_string());

    auto fragment = cc::result<loaded_stage>(loaded_stage{});
    if (desc.fragment_shader.has_value())
    {
        fragment = load_stage(_device, desc.fragment_shader.value(), "fragment");
        if (fragment.has_error())
        {
            vertex.value().function->release();
            vertex.value().library->release();
            return cc::error(fragment.error().to_string());
        }
    }

    auto const release_stages = [&]
    {
        if (fragment.value().function != nullptr)
        {
            fragment.value().function->release();
            fragment.value().library->release();
        }
        vertex.value().function->release();
        vertex.value().library->release();
    };

    auto vertex_descriptor = build_vertex_descriptor(desc.vertex_input);
    if (vertex_descriptor.has_error())
    {
        release_stages();
        return cc::error(vertex_descriptor.error().to_string());
    }

    auto* const descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunctionDescriptor(vertex.value().function);
    if (fragment.value().function != nullptr)
        descriptor->setFragmentFunctionDescriptor(fragment.value().function);
    if (vertex_descriptor.value() != nullptr)
        descriptor->setVertexDescriptor(vertex_descriptor.value());

    descriptor->setInputPrimitiveTopology(topology_class_of(desc.topology));
    descriptor->setRasterSampleCount(NS::UInteger(desc.sample_count < 1 ? 1 : desc.sample_count));

    // Rasterization is left on with no fragment stage: that is a depth-only pass, and Metal runs one correctly from
    // the vertex stage alone.
    // Disabling it there discards every primitive before the depth test, so the pass writes nothing at all.

    for (auto i = isize(0); i < desc.color_targets.size(); ++i)
    {
        auto const& target = desc.color_targets[i];
        auto* const attachment = descriptor->colorAttachments()->object(NS::UInteger(i));
        attachment->setPixelFormat(pixel_format_of(target.format));
        attachment->setWriteMask(color_write_mask_of(target.write_mask));

        if (target.blend.has_value())
        {
            auto const& blend = target.blend.value();
            attachment->setBlendingState(MTL4::BlendStateEnabled);
            attachment->setSourceRGBBlendFactor(blend_factor_of(blend.color.source));
            attachment->setDestinationRGBBlendFactor(blend_factor_of(blend.color.target));
            attachment->setRgbBlendOperation(blend_op_of(blend.color.op));
            attachment->setSourceAlphaBlendFactor(blend_factor_of(blend.alpha.source));
            attachment->setDestinationAlphaBlendFactor(blend_factor_of(blend.alpha.target));
            attachment->setAlphaBlendOperation(blend_op_of(blend.alpha.op));
        }
        else
        {
            attachment->setBlendingState(MTL4::BlendStateDisabled);
        }
    }

    NS::Error* pipeline_error = nullptr;
    auto const compile_guard = std::lock_guard(pipeline_compilation_lock());
    auto* const state = _compiler->newRenderPipelineState(descriptor, nullptr, &pipeline_error);

    descriptor->release();
    if (vertex_descriptor.value() != nullptr)
        vertex_descriptor.value()->release();
    release_stages();

    if (state == nullptr)
        return metal_error(pipeline_error, "raster_pipeline: the pipeline could not be built");

    // The depth and stencil test are a separate object here, bound on the encoder rather than baked into the pipeline.
    auto* const ds_descriptor = MTL::DepthStencilDescriptor::alloc()->init();
    auto const& ds = desc.depth_stencil;
    ds_descriptor->setDepthCompareFunction(ds.depth_test ? compare_of(ds.depth_compare) : MTL::CompareFunctionAlways);

    // A disabled depth test writes nothing, which is what dx12 and vulkan do: `depth_write` alone is not a licence to
    // write depth through a pass that declared it is not testing it.
    ds_descriptor->setDepthWriteEnabled(ds.depth_test && ds.depth_write);

    if (ds.stencil_test)
    {
        auto const face_state = [&](sg::stencil_face const& face)
        {
            auto* const out = MTL::StencilDescriptor::alloc()->init();
            out->setStencilCompareFunction(compare_of(face.compare));
            out->setStencilFailureOperation(stencil_op_of(face.fail));
            out->setDepthFailureOperation(stencil_op_of(face.depth_fail));
            out->setDepthStencilPassOperation(stencil_op_of(face.pass));
            out->setReadMask(u32(ds.stencil_read_mask));
            out->setWriteMask(u32(ds.stencil_write_mask));
            return out;
        };

        auto* const front = face_state(ds.front);
        auto* const back = face_state(ds.back);
        ds_descriptor->setFrontFaceStencil(front);
        ds_descriptor->setBackFaceStencil(back);
        front->release();
        back->release();
    }

    auto* const depth_stencil = _device->newDepthStencilState(ds_descriptor);
    ds_descriptor->release();
    if (depth_stencil == nullptr)
    {
        state->release();
        return cc::error("raster_pipeline: the metal device refused a depth-stencil state");
    }

    auto strides = cc::small_vector<isize, sg::max_vertex_buffers>();
    for (auto const& slot : desc.vertex_input.slots)
        strides.push_back(slot.stride);

    return std::make_shared<metal_raster_pipeline>(*this, state, depth_stencil, desc.rasterization, desc.topology,
                                                   desc.depth_stencil_format, cc::move(strides), desc.layout);
}
} // namespace sg::backend::metal
