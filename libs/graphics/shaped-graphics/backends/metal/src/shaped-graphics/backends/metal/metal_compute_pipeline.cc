#include "metal_compute_pipeline.hh"

#include <clean-core/string/format.hh>
#include <dispatch/dispatch.h>
#include <shaped-graphics/backends/metal/metal_binding_layout.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

#include <mutex>

namespace sg::backend::metal
{
void metal_compute_pipeline::release_backend_objects()
{
    if (_state == nullptr)
        return;

    auto* const state = _state;
    _state = nullptr;
    _ctx.epochs().defer([state] { state->release(); });
}

cc::result<metal_compute_pipeline_handle> metal_context::create_metal_compute_pipeline(
    sg::compute_pipeline_description const& desc,
    sg::lifetime_scope)
{
    auto const& shader = desc.shader;
    if (shader.stage != sg::shader_stage::compute)
        return cc::error("compute_pipeline: the shader is not a compute shader");

    auto const scope = autorelease_scope();

    auto loaded = library_from_shader(_device, shader, "compute_pipeline");
    if (loaded.has_error())
        return cc::error(cc::move(loaded).error());
    auto* const library = loaded.value();

    auto* const function_descriptor = MTL4::LibraryFunctionDescriptor::alloc()->init();
    function_descriptor->setLibrary(library);
    function_descriptor->setName(ns_string(shader.entry_point));

    auto* const pipeline_descriptor = MTL4::ComputePipelineDescriptor::alloc()->init();
    pipeline_descriptor->setComputeFunctionDescriptor(function_descriptor);

    NS::Error* pipeline_error = nullptr;
    auto const compile_guard = std::lock_guard(pipeline_compilation_lock());
    auto* const state = _compiler->newComputePipelineState(pipeline_descriptor, nullptr, &pipeline_error);

    pipeline_descriptor->release();
    function_descriptor->release();
    library->release();

    if (state == nullptr)
        return metal_error(pipeline_error, cc::format("compute_pipeline: '{}' could not be built", shader.entry_point));

    // The reflected workgroup size rather than the state's own: sg's dispatch_threads divides by what the shader
    // declared, and a pipeline whose reported size disagreed with the reflection would round differently.
    auto const workgroup = shader.workgroup_size.has_value() ? shader.workgroup_size.value() : sg::compute_dimensions{};

    return std::make_shared<metal_compute_pipeline>(*this, workgroup, state, desc.layout);
}
} // namespace sg::backend::metal
