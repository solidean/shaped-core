#include "metal_raytracing_pipeline.hh"

#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>
#include <dispatch/dispatch.h>
#include <shaped-graphics/backends/metal/metal_context.hh>

#include <mutex>

namespace sg::backend::metal
{
namespace
{
/// Load one single-entry metallib blob and mint the binary function its entry point names.
/// The library comes back through `out_libraries` because a binary function does not keep it alive.
[[nodiscard]] cc::result<MTL4::BinaryFunction*> make_binary_function(metal_context& ctx,
                                                                     sg::compiled_shader const& shader,
                                                                     sg::shader_stage expected,
                                                                     char const* what,
                                                                     cc::vector<MTL::Library*>& out_libraries)
{
    if (shader.stage != expected)
        return cc::error(cc::format("raytracing_pipeline: the {} shader has the wrong stage", what));
    if (shader.format != sg::shader_format::metal_lib)
        return cc::error(cc::format("raytracing_pipeline: the metal backend needs a metal_lib {} shader, got format {}",
                                    what, int(shader.format)));
    if (shader.bytecode.empty())
        return cc::error(cc::format("raytracing_pipeline: the {} shader has no bytecode", what));

    // DISPATCH_DATA_DESTRUCTOR_DEFAULT copies, so the pinned bytes need not outlive this call.
    auto* const blob = dispatch_data_create(shader.bytecode.data(), size_t(shader.bytecode.size()), nullptr,
                                            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NS::Error* library_error = nullptr;
    auto* const library = ctx.device()->newLibrary(blob, &library_error);
    dispatch_release(blob);

    if (library == nullptr)
        return metal_error(library_error,
                           cc::format("raytracing_pipeline: the {} metal library could not be loaded", what));
    out_libraries.push_back(library);

    auto* const function_descriptor = MTL4::LibraryFunctionDescriptor::alloc()->init();
    function_descriptor->setLibrary(library);
    function_descriptor->setName(ns_string(shader.entry_point));

    auto* const binary_descriptor = MTL4::BinaryFunctionDescriptor::alloc()->init();
    binary_descriptor->setFunctionDescriptor(function_descriptor);
    binary_descriptor->setName(ns_string(shader.entry_point));
    // Pipeline-independent, because the same function is linked into every raygen pipeline this description builds.
    binary_descriptor->setOptions(MTL4::BinaryFunctionOptionPipelineIndependent);

    NS::Error* function_error = nullptr;
    auto const compile_guard = std::lock_guard(pipeline_compilation_lock());
    auto* const function = ctx.compiler()->newBinaryFunction(binary_descriptor, nullptr, &function_error);

    binary_descriptor->release();
    function_descriptor->release();

    if (function == nullptr)
        return metal_error(function_error, cc::format("raytracing_pipeline: the {} function '{}' could not be linked",
                                                      what, shader.entry_point));
    return function;
}
} // namespace

void metal_raytracing_pipeline::release_backend_objects()
{
    if (_raygen_states.empty() && _libraries.empty())
        return;

    auto states = cc::move(_raygen_states);
    auto misses = cc::move(_miss_functions);
    auto callables = cc::move(_callable_functions);
    auto groups = cc::move(_hit_groups);
    auto libraries = cc::move(_libraries);
    _raygen_states.clear();
    _libraries.clear();

    _ctx.epochs().defer(
        [states = cc::move(states), misses = cc::move(misses), callables = cc::move(callables),
         groups = cc::move(groups), libraries = cc::move(libraries)]
        {
            for (auto* const s : states)
                if (s != nullptr)
                    s->release();
            for (auto* const f : misses)
                if (f != nullptr)
                    f->release();
            for (auto* const f : callables)
                if (f != nullptr)
                    f->release();
            for (auto const& g : groups)
            {
                if (g.intersection != nullptr)
                    g.intersection->release();
                if (g.any_hit != nullptr)
                    g.any_hit->release();
                if (g.closest_hit != nullptr)
                    g.closest_hit->release();
            }
            // Last: a binary function is minted from a library's function, so the library outlives every function.
            for (auto* const l : libraries)
                if (l != nullptr)
                    l->release();
        });
}

cc::result<sg::raytracing_pipeline_handle> metal_context::create_metal_raytracing_pipeline(
    sg::raytracing_pipeline_description const& desc,
    sg::lifetime_scope)
{
    if (desc.raygen_shaders.empty())
        return cc::error("raytracing_pipeline: at least one raygen shader is required");
    if (desc.max_recursion_depth < 1)
        return cc::error("raytracing_pipeline: max_recursion_depth must be >= 1");

    auto const scope = autorelease_scope();
    auto libraries = cc::vector<MTL::Library*>();

    // Everything is released through the pipeline once one exists; until then this unwinds a partial build.
    auto const unwind = [&](cc::vector<MTL4::BinaryFunction*> const& functions)
    {
        for (auto* const f : functions)
            if (f != nullptr)
                f->release();
        for (auto* const l : libraries)
            l->release();
    };

    auto linked = cc::vector<MTL4::BinaryFunction*>(); // every function linked into every raygen pipeline
    auto miss_functions = cc::vector<MTL4::BinaryFunction*>();
    auto callable_functions = cc::vector<MTL4::BinaryFunction*>();
    auto hit_groups = cc::vector<metal_raytracing_pipeline::hit_group>();

    for (auto const& shader : desc.miss_shaders)
    {
        auto function = make_binary_function(*this, shader, sg::shader_stage::miss, "miss", libraries);
        if (function.has_error())
        {
            unwind(linked);
            return cc::error(cc::move(function.error()));
        }
        miss_functions.push_back(function.value());
        linked.push_back(function.value());
    }

    for (auto const& shader : desc.callable_shaders)
    {
        auto function = make_binary_function(*this, shader, sg::shader_stage::callable, "callable", libraries);
        if (function.has_error())
        {
            unwind(linked);
            return cc::error(cc::move(function.error()));
        }
        callable_functions.push_back(function.value());
        linked.push_back(function.value());
    }

    for (auto const& group : desc.hit_shaders)
    {
        auto entry = metal_raytracing_pipeline::hit_group{};
        entry.is_procedural = group.intersection.has_value();

        // A member the group did not register stays null, which is a valid table entry rather than a failure.
        auto failure = cc::optional<cc::any_error>();
        auto const link = [&](cc::optional<sg::compiled_shader> const& shader, sg::shader_stage stage,
                              char const* what) -> MTL4::BinaryFunction*
        {
            if (!shader.has_value() || failure.has_value())
                return nullptr;
            auto function = make_binary_function(*this, shader.value(), stage, what, libraries);
            if (function.has_error())
            {
                failure = cc::move(function.error());
                return nullptr;
            }
            linked.push_back(function.value());
            return function.value();
        };

        entry.intersection = link(group.intersection, sg::shader_stage::intersection, "intersection");
        entry.any_hit = link(group.any_hit, sg::shader_stage::any_hit, "any-hit");
        entry.closest_hit = link(group.closest_hit, sg::shader_stage::closest_hit, "closest-hit");

        if (failure.has_value())
        {
            unwind(linked);
            return cc::error(cc::move(failure.value()));
        }

        hit_groups.push_back(entry);
    }

    // The linked set every raygen pipeline is built against.
    auto* const linked_array
        = NS::Array::array(reinterpret_cast<NS::Object* const*>(linked.data()), NS::UInteger(linked.size()));

    auto* const linking = MTL4::PipelineStageDynamicLinkingDescriptor::alloc()->init();
    linking->setBinaryLinkedFunctions(linked_array);

    // **This is what `max_recursion_depth` is for on Metal, and it must be set.**
    // Apple's own documentation for the property says to "change its value if you use recursive functions in your
    // compute pass", and that it covers indirect calls — visible functions, intersection functions, dynamic libraries.
    // **It defaults to 1**, so a shader that recurses deeper than the caller declared gets too little stack rather than
    // a diagnostic.
    //
    // The units differ from DXR's and the mapping is the conservative direction.
    // DXR counts TraceRay nesting; this counts indirect-call nesting, and a recursive trace ported to Metal spends at
    // least one indirect call per level — the hit function is reached through a visible function table.
    linking->setMaxCallStackDepth(NS::UInteger(desc.max_recursion_depth));

    auto raygen_states = cc::vector<MTL::ComputePipelineState*>();
    for (auto const& shader : desc.raygen_shaders)
    {
        if (shader.stage != sg::shader_stage::raygen)
        {
            unwind(linked);
            linking->release();
            return cc::error("raytracing_pipeline: a registered raygen shader has the wrong stage");
        }
        if (shader.format != sg::shader_format::metal_lib || shader.bytecode.empty())
        {
            unwind(linked);
            linking->release();
            return cc::error("raytracing_pipeline: the metal backend needs a non-empty metal_lib raygen shader");
        }

        auto* const blob = dispatch_data_create(shader.bytecode.data(), size_t(shader.bytecode.size()), nullptr,
                                                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NS::Error* library_error = nullptr;
        auto* const library = _device->newLibrary(blob, &library_error);
        dispatch_release(blob);

        if (library == nullptr)
        {
            unwind(linked);
            linking->release();
            return metal_error(library_error, "raytracing_pipeline: the raygen metal library could not be loaded");
        }
        libraries.push_back(library);

        auto* const function_descriptor = MTL4::LibraryFunctionDescriptor::alloc()->init();
        function_descriptor->setLibrary(library);
        function_descriptor->setName(ns_string(shader.entry_point));

        auto* const pipeline_descriptor = MTL4::ComputePipelineDescriptor::alloc()->init();
        pipeline_descriptor->setComputeFunctionDescriptor(function_descriptor);

        NS::Error* pipeline_error = nullptr;
        auto const compile_guard = std::lock_guard(pipeline_compilation_lock());
        auto* const state = _compiler->newComputePipelineState(pipeline_descriptor, linking, nullptr, &pipeline_error);

        pipeline_descriptor->release();
        function_descriptor->release();

        if (state == nullptr)
        {
            for (auto* const s : raygen_states)
                s->release();
            unwind(linked);
            linking->release();
            return metal_error(pipeline_error, cc::format("raytracing_pipeline: the raygen shader '{}' could not be "
                                                          "built",
                                                          shader.entry_point));
        }
        raygen_states.push_back(state);
    }

    linking->release();

    return sg::raytracing_pipeline_handle(std::make_shared<metal_raytracing_pipeline>(
        *this, cc::move(raygen_states), cc::move(miss_functions), cc::move(callable_functions), cc::move(hit_groups),
        cc::move(libraries), desc.layout));
}
} // namespace sg::backend::metal
