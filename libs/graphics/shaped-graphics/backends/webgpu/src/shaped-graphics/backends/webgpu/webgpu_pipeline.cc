// Compute and raster pipelines, built synchronously for the uncached tier and asynchronously for the cached one.

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/backends/webgpu/webgpu_format.hh>

namespace sg::backend::webgpu
{
namespace
{
[[nodiscard]] cc::result<wgpu_shader_module> create_module(webgpu_context& ctx,
                                                           sg::compiled_shader const& shader,
                                                           sg::shader_stage expected)
{
    ctx.assert_on_device_thread();
    if (shader.format != sg::shader_format::wgsl)
        return cc::error("webgpu builds pipelines from WGSL shaders only");
    if (shader.stage != expected)
        return cc::error(cc::format("a {} stage was given a shader compiled for another stage",
                                    expected == sg::shader_stage::compute  ? "compute"
                                    : expected == sg::shader_stage::vertex ? "vertex"
                                                                           : "fragment"));

    auto source = WGPUShaderSourceWGSL{};
    source.chain.next = nullptr;
    source.chain.sType = WGPUSType_ShaderSourceWGSL;
    source.code = WGPUStringView{.data = reinterpret_cast<char const*>(shader.bytecode.data()),
                                 .length = size_t(shader.bytecode.size())};
    auto const desc = WGPUShaderModuleDescriptor{
        .nextInChain = &source.chain,
        .label = to_wgpu(shader.entry_point),
    };
    auto module = wgpu_shader_module(wgpuDeviceCreateShaderModule(ctx.device(), &desc));
    if (!module)
        return cc::error("wgpuDeviceCreateShaderModule returned no module");
    return module;
}

[[nodiscard]] webgpu_pipeline_layout_handle as_webgpu_layout(sg::pipeline_layout_handle const& layout)
{
    CC_ASSERT(layout != nullptr, "a pipeline requires a pipeline layout");
    auto webgpu_layout = std::dynamic_pointer_cast<webgpu_pipeline_layout const>(layout);
    CC_ASSERT(webgpu_layout != nullptr, "pipeline layout is not a webgpu one");
    return webgpu_layout;
}

[[nodiscard]] WGPUVertexFormat to_wgpu_vertex_format(sg::vertex_attribute_format f)
{
    switch (f)
    {
    case sg::vertex_attribute_format::f32:
        return WGPUVertexFormat_Float32;
    case sg::vertex_attribute_format::vec2f:
        return WGPUVertexFormat_Float32x2;
    case sg::vertex_attribute_format::vec3f:
        return WGPUVertexFormat_Float32x3;
    case sg::vertex_attribute_format::vec4f:
        return WGPUVertexFormat_Float32x4;
    case sg::vertex_attribute_format::i32:
        return WGPUVertexFormat_Sint32;
    case sg::vertex_attribute_format::vec2i:
        return WGPUVertexFormat_Sint32x2;
    case sg::vertex_attribute_format::vec3i:
        return WGPUVertexFormat_Sint32x3;
    case sg::vertex_attribute_format::vec4i:
        return WGPUVertexFormat_Sint32x4;
    case sg::vertex_attribute_format::u32:
        return WGPUVertexFormat_Uint32;
    case sg::vertex_attribute_format::vec2u:
        return WGPUVertexFormat_Uint32x2;
    case sg::vertex_attribute_format::vec3u:
        return WGPUVertexFormat_Uint32x3;
    case sg::vertex_attribute_format::vec4u:
        return WGPUVertexFormat_Uint32x4;
    case sg::vertex_attribute_format::rgba8_unorm:
        return WGPUVertexFormat_Unorm8x4;
    case sg::vertex_attribute_format::rgba8_uint:
        return WGPUVertexFormat_Uint8x4;
    }
    CC_UNREACHABLE("unhandled vertex_attribute_format");
}

[[nodiscard]] WGPUBlendFactor to_wgpu_blend_factor(sg::blend_factor f)
{
    switch (f)
    {
    case sg::blend_factor::zero:
        return WGPUBlendFactor_Zero;
    case sg::blend_factor::one:
        return WGPUBlendFactor_One;
    case sg::blend_factor::src_color:
        return WGPUBlendFactor_Src;
    case sg::blend_factor::one_minus_src_color:
        return WGPUBlendFactor_OneMinusSrc;
    case sg::blend_factor::dst_color:
        return WGPUBlendFactor_Dst;
    case sg::blend_factor::one_minus_dst_color:
        return WGPUBlendFactor_OneMinusDst;
    case sg::blend_factor::src_alpha:
        return WGPUBlendFactor_SrcAlpha;
    case sg::blend_factor::one_minus_src_alpha:
        return WGPUBlendFactor_OneMinusSrcAlpha;
    case sg::blend_factor::dst_alpha:
        return WGPUBlendFactor_DstAlpha;
    case sg::blend_factor::one_minus_dst_alpha:
        return WGPUBlendFactor_OneMinusDstAlpha;
    }
    CC_UNREACHABLE("unhandled blend_factor");
}

[[nodiscard]] WGPUBlendOperation to_wgpu_blend_op(sg::blend_op op)
{
    switch (op)
    {
    case sg::blend_op::add:
        return WGPUBlendOperation_Add;
    case sg::blend_op::subtract:
        return WGPUBlendOperation_Subtract;
    case sg::blend_op::reverse_subtract:
        return WGPUBlendOperation_ReverseSubtract;
    case sg::blend_op::min:
        return WGPUBlendOperation_Min;
    case sg::blend_op::max:
        return WGPUBlendOperation_Max;
    }
    CC_UNREACHABLE("unhandled blend_op");
}

[[nodiscard]] WGPUBlendComponent to_wgpu_blend_component(sg::blend_component const& c)
{
    // WebGPU requires both factors of a min or max blend to be one, and ignores them either way.
    auto const is_extremum = c.op == sg::blend_op::min || c.op == sg::blend_op::max;
    return WGPUBlendComponent{
        .operation = to_wgpu_blend_op(c.op),
        .srcFactor = is_extremum ? WGPUBlendFactor_One : to_wgpu_blend_factor(c.source),
        .dstFactor = is_extremum ? WGPUBlendFactor_One : to_wgpu_blend_factor(c.target),
    };
}

[[nodiscard]] WGPUStencilOperation to_wgpu_stencil_op(sg::stencil_op op)
{
    switch (op)
    {
    case sg::stencil_op::keep:
        return WGPUStencilOperation_Keep;
    case sg::stencil_op::zero:
        return WGPUStencilOperation_Zero;
    case sg::stencil_op::replace:
        return WGPUStencilOperation_Replace;
    case sg::stencil_op::increment_clamp:
        return WGPUStencilOperation_IncrementClamp;
    case sg::stencil_op::decrement_clamp:
        return WGPUStencilOperation_DecrementClamp;
    case sg::stencil_op::invert:
        return WGPUStencilOperation_Invert;
    case sg::stencil_op::increment_wrap:
        return WGPUStencilOperation_IncrementWrap;
    case sg::stencil_op::decrement_wrap:
        return WGPUStencilOperation_DecrementWrap;
    }
    CC_UNREACHABLE("unhandled stencil_op");
}

[[nodiscard]] WGPUStencilFaceState to_wgpu_stencil_face(sg::stencil_face const& f, bool enabled)
{
    if (!enabled)
        return WGPUStencilFaceState{.compare = WGPUCompareFunction_Always,
                                    .failOp = WGPUStencilOperation_Keep,
                                    .depthFailOp = WGPUStencilOperation_Keep,
                                    .passOp = WGPUStencilOperation_Keep};
    return WGPUStencilFaceState{
        .compare = to_wgpu_compare(f.compare),
        .failOp = to_wgpu_stencil_op(f.fail),
        .depthFailOp = to_wgpu_stencil_op(f.depth_fail),
        .passOp = to_wgpu_stencil_op(f.pass),
    };
}

[[nodiscard]] WGPUPrimitiveTopology to_wgpu_topology(sg::primitive_topology t)
{
    switch (t)
    {
    case sg::primitive_topology::point_list:
        return WGPUPrimitiveTopology_PointList;
    case sg::primitive_topology::line_list:
        return WGPUPrimitiveTopology_LineList;
    case sg::primitive_topology::line_strip:
        return WGPUPrimitiveTopology_LineStrip;
    case sg::primitive_topology::triangle_list:
        return WGPUPrimitiveTopology_TriangleList;
    case sg::primitive_topology::triangle_strip:
        return WGPUPrimitiveTopology_TriangleStrip;
    case sg::primitive_topology::patch_list:
        break;
    }
    CC_UNREACHABLE("patch lists are refused before translation");
}
} // namespace

// -- compute --

struct compute_pipeline_build
{
    webgpu_pipeline_layout_handle layout;
    wgpu_shader_module module;
    cc::string entry_point;
    sg::compute_dimensions workgroup_size;
    WGPUComputePipelineDescriptor descriptor = {};
};

cc::result<std::unique_ptr<compute_pipeline_build>> prepare_compute_pipeline(webgpu_context& ctx,
                                                                             sg::compute_pipeline_description const& desc)
{
    auto build = std::make_unique<compute_pipeline_build>();
    build->layout = as_webgpu_layout(desc.layout);
    auto module = create_module(ctx, desc.shader, sg::shader_stage::compute);
    CC_RETURN_IF_ERROR(module);
    build->module = cc::move(module.value());
    build->entry_point = desc.shader.entry_point;
    build->workgroup_size = desc.shader.workgroup_size.value_or(sg::compute_dimensions{});

    build->descriptor.label = to_wgpu("sg compute pipeline");
    build->descriptor.layout = build->layout->raw();
    build->descriptor.compute.module = build->module.get();
    build->descriptor.compute.entryPoint = to_wgpu(build->entry_point);
    return build;
}

WGPUComputePipelineDescriptor const& descriptor_of(compute_pipeline_build const& build)
{
    return build.descriptor;
}

webgpu_compute_pipeline_handle finish_compute_pipeline(compute_pipeline_build& build, wgpu_compute_pipeline pipeline)
{
    return std::make_shared<webgpu_compute_pipeline>(build.workgroup_size, build.layout, cc::move(pipeline));
}

cc::result<sg::compute_pipeline_handle> webgpu_context::try_create_compute_pipeline(
    sg::compute_pipeline_description const& desc,
    sg::lifetime_scope scope)
{
    CC_ASSERT(scope == sg::lifetime_scope::persistent, "pipelines are persistent-only");
    auto build = prepare_compute_pipeline(*this, desc);
    CC_RETURN_IF_ERROR(build);
    auto pipeline = wgpu_compute_pipeline(wgpuDeviceCreateComputePipeline(device(), &descriptor_of(*build.value())));
    if (!pipeline)
        return cc::error("wgpuDeviceCreateComputePipeline returned no pipeline");
    return sg::compute_pipeline_handle(finish_compute_pipeline(*build.value(), cc::move(pipeline)));
}

// -- raster --

struct raster_pipeline_build
{
    webgpu_pipeline_layout_handle layout;
    wgpu_shader_module vertex_module;
    wgpu_shader_module fragment_module;
    cc::string vertex_entry;
    cc::string fragment_entry;
    cc::vector<cc::vector<WGPUVertexAttribute>> attributes;
    cc::vector<WGPUVertexBufferLayout> vertex_buffers;
    cc::vector<WGPUBlendState> blends;
    cc::vector<WGPUColorTargetState> color_targets;
    WGPUDepthStencilState depth_stencil = {};
    WGPUFragmentState fragment = {};
    WGPURenderPipelineDescriptor descriptor = {};
};

cc::result<std::unique_ptr<raster_pipeline_build>> prepare_raster_pipeline(webgpu_context& ctx,
                                                                           sg::raster_pipeline_description const& desc)
{
    if (desc.geometry_shader.has_value())
        return cc::error("webgpu has no geometry stage (ctx.supports(sg::feature::geometry_shader) is false)");
    if (desc.tessellation_control_shader.has_value() || desc.tessellation_evaluation_shader.has_value()
        || desc.topology == sg::primitive_topology::patch_list)
        return cc::error("webgpu has no tessellation stages (ctx.supports(sg::feature::tessellation_shader) is false)");
    if (desc.rasterization.fill == sg::fill_mode::wireframe)
        return cc::error("webgpu has no wireframe fill mode");
    if (desc.sample_count != 1 && desc.sample_count != 4)
        return cc::error(cc::format("webgpu supports a sample count of 1 or 4, not {}", desc.sample_count));

    auto build = std::make_unique<raster_pipeline_build>();
    build->layout = as_webgpu_layout(desc.layout);

    auto vertex_module = create_module(ctx, desc.vertex_shader, sg::shader_stage::vertex);
    CC_RETURN_IF_ERROR(vertex_module);
    build->vertex_module = cc::move(vertex_module.value());
    build->vertex_entry = desc.vertex_shader.entry_point;

    // One attribute list per slot; an attribute's @location is its index in the layout's flat list.
    auto const& input = desc.vertex_input;
    build->attributes.resize_to_defaulted(input.slots.size());
    for (isize i = 0; i < input.attributes.size(); ++i)
    {
        auto const& a = input.attributes[i];
        CC_ASSERT(a.slot >= 0 && a.slot < int(input.slots.size()), "a vertex attribute names a slot the layout lacks");
        build->attributes[a.slot].push_back(WGPUVertexAttribute{
            .nextInChain = nullptr,
            .format = to_wgpu_vertex_format(a.format),
            .offset = u64(a.offset),
            .shaderLocation = u32(i),
        });
    }
    for (isize s = 0; s < input.slots.size(); ++s)
        build->vertex_buffers.push_back(WGPUVertexBufferLayout{
            .nextInChain = nullptr,
            .stepMode = input.slots[s].per_instance ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex,
            .arrayStride = u64(input.slots[s].stride),
            .attributeCount = size_t(build->attributes[s].size()),
            .attributes = build->attributes[s].empty() ? nullptr : build->attributes[s].data(),
        });

    auto& d = build->descriptor;
    d.label = to_wgpu("sg raster pipeline");
    d.layout = build->layout->raw();
    d.vertex.module = build->vertex_module.get();
    d.vertex.entryPoint = to_wgpu(build->vertex_entry);
    d.vertex.bufferCount = size_t(build->vertex_buffers.size());
    d.vertex.buffers = build->vertex_buffers.empty() ? nullptr : build->vertex_buffers.data();

    d.primitive.topology = to_wgpu_topology(desc.topology);
    d.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
    d.primitive.frontFace
        = desc.rasterization.front == sg::front_face::counter_clockwise ? WGPUFrontFace_CCW : WGPUFrontFace_CW;
    d.primitive.cullMode = desc.rasterization.cull == sg::cull_mode::none  ? WGPUCullMode_None
                         : desc.rasterization.cull == sg::cull_mode::front ? WGPUCullMode_Front
                                                                           : WGPUCullMode_Back;
    if (!desc.rasterization.depth_clip_enabled)
    {
        if (!wgpuDeviceHasFeature(ctx.device(), WGPUFeatureName_DepthClipControl))
            return cc::error("disabling depth clipping needs the depth-clip-control feature, which this device lacks");
        d.primitive.unclippedDepth = WGPU_TRUE;
    }

    if (desc.depth_stencil_format != sg::pixel_format::undefined)
    {
        auto const& ds = desc.depth_stencil;
        auto& s = build->depth_stencil;
        s.format = to_wgpu_format(desc.depth_stencil_format);
        s.depthWriteEnabled = ds.depth_test && ds.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        s.depthCompare = ds.depth_test ? to_wgpu_compare(ds.depth_compare) : WGPUCompareFunction_Always;
        s.stencilFront = to_wgpu_stencil_face(ds.stencil_front, ds.stencil_test);
        s.stencilBack = to_wgpu_stencil_face(ds.stencil_back, ds.stencil_test);
        s.stencilReadMask = ds.stencil_test ? ds.stencil_read_mask : 0xFF;
        s.stencilWriteMask = ds.stencil_test ? ds.stencil_write_mask : 0;
        s.depthBias = i32(desc.rasterization.depth_bias);
        s.depthBiasSlopeScale = desc.rasterization.depth_bias_slope;
        s.depthBiasClamp = desc.rasterization.depth_bias_clamp;
        d.depthStencil = &build->depth_stencil;
    }

    d.multisample.count = u32(desc.sample_count);
    d.multisample.mask = 0xFFFFFFFF;
    d.multisample.alphaToCoverageEnabled = WGPU_FALSE;

    if (desc.fragment_shader.has_value())
    {
        auto fragment_module = create_module(ctx, desc.fragment_shader.value(), sg::shader_stage::fragment);
        CC_RETURN_IF_ERROR(fragment_module);
        build->fragment_module = cc::move(fragment_module.value());
        build->fragment_entry = desc.fragment_shader.value().entry_point;

        // Reserved first, so the pointers into it stay put.
        build->blends.reserve(desc.color_targets.size());
        for (auto const& t : desc.color_targets)
        {
            auto target = WGPUColorTargetState{};
            target.format = to_wgpu_format(t.format);
            if (t.blend.has_value())
            {
                build->blends.push_back(WGPUBlendState{
                    .color = to_wgpu_blend_component(t.blend.value().color),
                    .alpha = to_wgpu_blend_component(t.blend.value().alpha),
                });
                target.blend = &build->blends.back();
            }
            auto mask = WGPUColorWriteMask_None;
            if (t.write_mask.has(sg::color_channel::r))
                mask |= WGPUColorWriteMask_Red;
            if (t.write_mask.has(sg::color_channel::g))
                mask |= WGPUColorWriteMask_Green;
            if (t.write_mask.has(sg::color_channel::b))
                mask |= WGPUColorWriteMask_Blue;
            if (t.write_mask.has(sg::color_channel::a))
                mask |= WGPUColorWriteMask_Alpha;
            target.writeMask = mask;
            build->color_targets.push_back(target);
        }

        build->fragment.module = build->fragment_module.get();
        build->fragment.entryPoint = to_wgpu(build->fragment_entry);
        build->fragment.targetCount = size_t(build->color_targets.size());
        build->fragment.targets = build->color_targets.empty() ? nullptr : build->color_targets.data();
        d.fragment = &build->fragment;
    }
    else if (!desc.color_targets.empty())
        return cc::error("a raster pipeline with color targets needs a fragment shader");

    return build;
}

WGPURenderPipelineDescriptor const& descriptor_of(raster_pipeline_build const& build)
{
    return build.descriptor;
}

webgpu_raster_pipeline_handle finish_raster_pipeline(raster_pipeline_build& build, wgpu_render_pipeline pipeline)
{
    return std::make_shared<webgpu_raster_pipeline>(build.layout, cc::move(pipeline));
}

cc::result<sg::raster_pipeline_handle> webgpu_context::try_create_raster_pipeline(sg::raster_pipeline_description const& desc,
                                                                                  sg::lifetime_scope scope)
{
    CC_ASSERT(scope == sg::lifetime_scope::persistent, "pipelines are persistent-only");
    auto build = prepare_raster_pipeline(*this, desc);
    CC_RETURN_IF_ERROR(build);
    auto pipeline = wgpu_render_pipeline(wgpuDeviceCreateRenderPipeline(device(), &descriptor_of(*build.value())));
    if (!pipeline)
        return cc::error("wgpuDeviceCreateRenderPipeline returned no pipeline");
    return sg::raster_pipeline_handle(finish_raster_pipeline(*build.value(), cc::move(pipeline)));
}

// -- asynchronous builds --

namespace
{
template <class Build, class Handle>
struct pipeline_request
{
    std::shared_ptr<webgpu_callback_anchor> anchor;
    std::unique_ptr<Build> build;
    cc::shared_async<Handle> node;
};

[[nodiscard]] webgpu_compute_pipeline_handle finish_pipeline(compute_pipeline_build& build, wgpu_compute_pipeline p)
{
    return finish_compute_pipeline(build, cc::move(p));
}

[[nodiscard]] webgpu_raster_pipeline_handle finish_pipeline(raster_pipeline_build& build, wgpu_render_pipeline p)
{
    return finish_raster_pipeline(build, cc::move(p));
}

template <class Handle, class Start>
[[nodiscard]] cc::shared_async<Handle> started_on(cc::async_scheduler& home, Start start)
{
    co_await cc::async_resume_on(home);
    auto const inner = start();
    co_return co_await inner;
}

/// A build asked for off the device thread, started once a coroutine has moved there: WebGPU exists on that thread only.
template <class Handle, class Start>
[[nodiscard]] cc::shared_async<Handle> start_on_device_home(webgpu_context const& ctx, Start start)
{
    CC_ASSERT(ctx.device_home() != nullptr, "an async pipeline build was asked for off the device thread, and the "
                                            "device has no home to move it to");
    return started_on<Handle>(*ctx.device_home(), cc::move(start));
}

template <class Build, class Handle, class Raw, class Wrap>
void settle_pipeline(WGPUCreatePipelineAsyncStatus status, Raw pipeline, WGPUStringView message, void* userdata1)
{
    auto const request
        = std::unique_ptr<pipeline_request<Build, Handle>>(static_cast<pipeline_request<Build, Handle>*>(userdata1));
    auto owned = Wrap(pipeline);
    if (status != WGPUCreatePipelineAsyncStatus_Success || request->anchor->ctx == nullptr)
    {
        auto const why = request->anchor->ctx == nullptr
                           ? cc::string("the context shut down before the pipeline was built")
                           : from_wgpu(message);
        request->node->push_error(cc::async_error::make_error(cc::any_error(why)));
        return;
    }
    request->node->push_value(Handle(finish_pipeline(*request->build, cc::move(owned))));
}
} // namespace

cc::shared_async<sg::compute_pipeline_handle> webgpu_context::create_compute_pipeline_async(
    sg::compute_pipeline_description const& desc,
    sg::lifetime_scope scope)
{
    CC_ASSERT(scope == sg::lifetime_scope::persistent, "pipelines are persistent-only");
    if (!is_on_device_thread())
        return start_on_device_home<sg::compute_pipeline_handle>(
            *this,
            [anchor = _anchor, desc, scope]() -> cc::shared_async<sg::compute_pipeline_handle>
            {
                if (anchor->ctx == nullptr)
                    return cc::make_async_from_error<sg::compute_pipeline_handle>(cc::async_error::make_error(
                        cc::any_error("the context shut down before the pipeline was built")));
                return anchor->ctx->create_compute_pipeline_async(desc, scope);
            });

    auto build = prepare_compute_pipeline(*this, desc);
    if (build.has_error())
        return cc::make_async_from_error<sg::compute_pipeline_handle>(
            cc::async_error::make_error(cc::move(build.error())));

    auto node = cc::make_async_manual<sg::compute_pipeline_handle>();
    auto request = std::make_unique<pipeline_request<compute_pipeline_build, sg::compute_pipeline_handle>>();
    request->anchor = _anchor;
    request->build = cc::move(build.value());
    request->node = node;
    auto const& descriptor = descriptor_of(*request->build);
    auto const info = WGPUCreateComputePipelineAsyncCallbackInfo{
        .nextInChain = nullptr,
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback =
            [](WGPUCreatePipelineAsyncStatus status, WGPUComputePipeline pipeline, WGPUStringView message, void* u1, void*)
        {
            settle_pipeline<compute_pipeline_build, sg::compute_pipeline_handle, WGPUComputePipeline, wgpu_compute_pipeline>(
                status, pipeline, message, u1);
        },
        .userdata1 = request.release(),
        .userdata2 = nullptr,
    };
    (void)wgpuDeviceCreateComputePipelineAsync(device(), &descriptor, info);
    return node;
}

cc::shared_async<sg::raster_pipeline_handle> webgpu_context::create_raster_pipeline_async(
    sg::raster_pipeline_description const& desc,
    sg::lifetime_scope scope)
{
    CC_ASSERT(scope == sg::lifetime_scope::persistent, "pipelines are persistent-only");
    if (!is_on_device_thread())
        return start_on_device_home<sg::raster_pipeline_handle>(
            *this,
            [anchor = _anchor, desc, scope]() -> cc::shared_async<sg::raster_pipeline_handle>
            {
                if (anchor->ctx == nullptr)
                    return cc::make_async_from_error<sg::raster_pipeline_handle>(cc::async_error::make_error(
                        cc::any_error("the context shut down before the pipeline was built")));
                return anchor->ctx->create_raster_pipeline_async(desc, scope);
            });

    auto build = prepare_raster_pipeline(*this, desc);
    if (build.has_error())
        return cc::make_async_from_error<sg::raster_pipeline_handle>(cc::async_error::make_error(cc::move(build.error())));

    auto node = cc::make_async_manual<sg::raster_pipeline_handle>();
    auto request = std::make_unique<pipeline_request<raster_pipeline_build, sg::raster_pipeline_handle>>();
    request->anchor = _anchor;
    request->build = cc::move(build.value());
    request->node = node;
    auto const& descriptor = descriptor_of(*request->build);
    auto const info = WGPUCreateRenderPipelineAsyncCallbackInfo{
        .nextInChain = nullptr,
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback =
            [](WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline, WGPUStringView message, void* u1, void*)
        {
            settle_pipeline<raster_pipeline_build, sg::raster_pipeline_handle, WGPURenderPipeline, wgpu_render_pipeline>(
                status, pipeline, message, u1);
        },
        .userdata1 = request.release(),
        .userdata2 = nullptr,
    };
    (void)wgpuDeviceCreateRenderPipelineAsync(device(), &descriptor, info);
    return node;
}
} // namespace sg::backend::webgpu
