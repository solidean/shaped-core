#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>

#include <memory>

// A shader's print and assert, end to end: SGL writes the records, sg binds the window, reads it back once the epoch
// completes, and logs what it decoded.
// Here because this is the lowest test binary with a context of every backend and a compiler to feed it.

namespace
{
constexpr cc::string_view k_source = "@compute(4) fun cs(@thread_id id: int3):\n"
                                     "    print \"thread $(id.x)\"\n"
                                     "    assert id.x != 3, \"thread $(id.x) of $(id.xy) is the odd one\"\n";

/// The compiler that builds SGL's text for `ctx`, where sg binds the debug window on its backend; null elsewhere.
std::unique_ptr<slib::shader_compiler> inner_compiler_for(sg::context const& ctx)
{
    if (ctx.backend() == sg::backend_kind::webgpu)
        return slib::create_wgsl_compiler();
#if SLIB_HAS_DXC
    if (ctx.backend() == sg::backend_kind::dx12 || ctx.backend() == sg::backend_kind::vulkan)
    {
        auto dxc
            = ctx.backend() == sg::backend_kind::dx12 ? slib::create_dxc_compiler() : slib::create_dxc_spirv_compiler();
        if (dxc.has_value())
            return cc::move(dxc.value());
    }
#endif
    return nullptr;
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - a shader's failed assert reaches the log with its values, once its epoch completes",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    auto inner = inner_compiler_for(ctx);
    if (inner == nullptr)
        SKIP("sg binds no debug window on this backend yet");
    nx::expect_error("debug.sgl:3: assert failed: thread 3 of int2(3, 0) is the odd one", nx::exactly(1));

    auto const compiler = slib::create_sgl_compiler(cc::move(inner), {.persistent_cache = nullptr, .diagnostics = true});

    auto desc = slib::shader_source_description{.source = cc::string(k_source),
                                                .entry_point = "cs",
                                                .stage = sg::shader_stage::compute,
                                                .label = "debug.sgl"};
    auto preprocessed = compiler->preprocess(desc, [](cc::string_view) { return cc::optional<cc::string>(); });
    REQUIRE(preprocessed.has_value());
    REQUIRE(preprocessed.value().stated.has_value());
    auto shader = preprocessed.value().stated.value();
    CHECK(shader.debug_sites.size() == 2);

    desc.source = preprocessed.value().source;
    if (!preprocessed.value().entry_point.empty())
        desc.entry_point = preprocessed.value().entry_point;
    auto const compiled = co_await compiler->compile(desc);
    shader.bytecode = compiled.bytecode;
    shader.compiler = compiled.compiler;

    auto const layout = ctx.uncached.create_pipeline_layout({});
    auto const pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = layout});

    auto cmd = ctx.create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.dispatch_threads(4);
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    // the window is asked for once the epoch retires, and decoded once its bytes are back
    co_await ctx.idle_completion();
    co_await ctx.idle_completion();
    ctx.process_completed_epochs();
    // and the window's reset, which decoding it queued, lands before the test ends
    co_await ctx.idle_completion();
}
