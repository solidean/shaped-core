#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#if SLIB_HAS_METAL
#include <shaped-shader-library/compiler/metal_compiler.hh>
#endif
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>

#include <memory>

// A shader's print and assert, end to end: SGL writes the records, sg binds the window, reads it back once the epoch
// completes, and logs what it decoded.
// Here because this is the lowest test binary with a context of every backend and a compiler to feed it.

using namespace cc::primitive_defines;

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
#if SLIB_HAS_METAL
    if (ctx.backend() == sg::backend_kind::metal)
        return slib::create_metal_compiler();
#endif
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

/// `source`'s `cs` compiled for `ctx` under `guard`, its group's layout, and a group of the buffer `written` and of the
/// binding array `views`, ready for a dispatch of 4 threads that writes 4 uints into `written`.
struct guarded_compute
{
    sg::compute_pipeline_handle pipeline;
    sg::binding_group_handle group;
};

cc::shared_async<guarded_compute> guarded_compute_of(sg::context& ctx,
                                                     cc::string source,
                                                     cc::string label,
                                                     slib::sgl_undefined_guard guard,
                                                     sg::buffer<u32> const& written,
                                                     cc::vector<sg::raw_view> views)
{
    auto const compiler = slib::create_sgl_compiler(
        inner_compiler_for(ctx), {.persistent_cache = nullptr, .diagnostics = true, .guards_undefined = guard});
    auto desc = slib::shader_source_description{.source = cc::string(source),
                                                .entry_point = "cs",
                                                .stage = sg::shader_stage::compute,
                                                .label = cc::string(label)};
    // what a host's compile takes of its context (EMIT-245), so a native context runs SGL's own binary
    slib::add_context_options(desc.options, ctx);
    auto preprocessed = compiler->preprocess(desc, [](cc::string_view) { return cc::optional<cc::string>(); });
    REQUIRE(preprocessed.has_value());
    REQUIRE(preprocessed.value().stated.has_value());
    auto shader = preprocessed.value().stated.value();

    desc.source = preprocessed.value().source;
    desc.bytecode = cc::move(preprocessed.value().bytecode);
    if (!preprocessed.value().entry_point.empty())
        desc.entry_point = preprocessed.value().entry_point;
    auto const compiled = co_await compiler->compile(desc);
    shader.bytecode = compiled.bytecode;
    shader.compiler = compiled.compiler;

    auto const group_layout = ctx.uncached.create_binding_group_layout(shader.bindings);
    auto layout_groups = cc::small_vector<sg::binding_group_layout_handle, sg::max_binding_groups>();
    layout_groups.push_back(group_layout);
    auto const layout = ctx.uncached.create_pipeline_layout({.groups = layout_groups});

    auto named = cc::vector<sg::named_view>();
    for (auto const& b : shader.bindings)
        if (b.count > 1 || b.is_unbounded())
            named.push_back({.name = b.name, .view = cc::move(views)});
        else
            named.push_back({.name = b.name, .view = written.as_readwrite_buffer()});
    co_return guarded_compute{.pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = layout}),
                              .group = ctx.persistent.create_binding_group(group_layout, named)};
}
/// `source`'s entry point `entry`, a `stage`, compiled for `ctx` under `guard`.
cc::shared_async<sg::compiled_shader> guarded_stage_of(sg::context& ctx,
                                                       cc::string source,
                                                       cc::string entry,
                                                       sg::shader_stage stage,
                                                       cc::string label,
                                                       slib::sgl_undefined_guard guard)
{
    auto const compiler = slib::create_sgl_compiler(
        inner_compiler_for(ctx), {.persistent_cache = nullptr, .diagnostics = true, .guards_undefined = guard});
    auto desc = slib::shader_source_description{.source = cc::move(source),
                                                .entry_point = cc::move(entry),
                                                .stage = stage,
                                                .label = cc::move(label)};
    slib::add_context_options(desc.options, ctx);
    auto preprocessed = compiler->preprocess(desc, [](cc::string_view) { return cc::optional<cc::string>(); });
    REQUIRE(preprocessed.has_value());
    REQUIRE(preprocessed.value().stated.has_value());
    auto shader = preprocessed.value().stated.value();
    desc.source = preprocessed.value().source;
    desc.bytecode = cc::move(preprocessed.value().bytecode);
    if (!preprocessed.value().entry_point.empty())
        desc.entry_point = preprocessed.value().entry_point;
    auto const compiled = co_await compiler->compile(desc);
    shader.bytecode = compiled.bytecode;
    shader.compiler = compiled.compiler;
    co_return shader;
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
    slib::add_context_options(desc.options, ctx);
    auto preprocessed = compiler->preprocess(desc, [](cc::string_view) { return cc::optional<cc::string>(); });
    REQUIRE(preprocessed.has_value());
    REQUIRE(preprocessed.value().stated.has_value());
    auto shader = preprocessed.value().stated.value();
    CHECK(shader.debug_sites.size() == 2);

    desc.source = preprocessed.value().source;
    desc.bytecode = cc::move(preprocessed.value().bytecode);
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

ASYNC_INVOCABLE_TEST("sr - a shader's division by zero is reported as a failed assert where it would have run, and "
                     "that "
                     "invocation ends, past a barrier where one follows (LEGAL-65)",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    if (inner_compiler_for(ctx) == nullptr)
        SKIP("sg binds no debug window on this backend yet");
    // thread 2 divides by zero: once at once, and once with a barrier behind it that every thread must reach
    nx::expect_error("guard.sgl:2: assert failed: `12 / 0` has no value", nx::exactly(2));

    cc::string_view const sources[] = {
        "@compute(4) fun cs(@thread_id id: int3):\n"
        "    let q = 12 / (id.x - 2)\n"
        "    print \"thread $(id.x): $q\"\n",
        "@compute(4) fun cs(@thread_id id: int3):\n"
        "    let q = 12 / (id.x - 2)\n"
        "    workgroup_barrier()\n"
        "    print \"thread $(id.x): $q\"\n",
    };
    for (auto const source : sources)
    {
        auto const compiler = slib::create_sgl_compiler(
            inner_compiler_for(ctx),
            {.persistent_cache = nullptr, .diagnostics = true, .guards_undefined = slib::sgl_undefined_guard::report});
        auto desc = slib::shader_source_description{.source = cc::string(source),
                                                    .entry_point = "cs",
                                                    .stage = sg::shader_stage::compute,
                                                    .label = "guard.sgl"};
        slib::add_context_options(desc.options, ctx);
        auto preprocessed = compiler->preprocess(desc, [](cc::string_view) { return cc::optional<cc::string>(); });
        REQUIRE(preprocessed.has_value());
        REQUIRE(preprocessed.value().stated.has_value());
        auto shader = preprocessed.value().stated.value();

        desc.source = preprocessed.value().source;
        desc.bytecode = cc::move(preprocessed.value().bytecode);
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
        co_await ctx.epochs_in_flight_completion(2);
        ctx.advance_epoch();

        co_await ctx.idle_completion();
        co_await ctx.idle_completion();
        ctx.process_completed_epochs();
        co_await ctx.idle_completion();
    }
}

ASYNC_INVOCABLE_TEST("sr - a read of workgroup memory no invocation stored is reported where it would have run, and "
                     "a read of what another stored is not (LEGAL-69)",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    if (inner_compiler_for(ctx) == nullptr)
        SKIP("sg binds no debug window on this backend yet");
    // thread 2 stores nothing, so thread 1 reads a hole; the second shader stores every element first
    nx::expect_error("shadow.sgl:8: assert failed: workgroup memory `tile.values[2]` is read before any invocation "
                     "stored it",
                     nx::exactly(1));

    cc::string_view const sources[] = {
        "@workgroup binding tile:\n"
        "    values: float[4]\n"
        "\n"
        "@compute(4) fun cs(@local_thread_index li: int){tile}:\n"
        "    if li != 2:\n"
        "        tile.values[li] = float(li)\n"
        "    workgroup_barrier()\n"
        "    print \"thread $li: $(tile.values[(li + 1) % 4])\"\n",
        "@workgroup binding tile:\n"
        "    values: float[4]\n"
        "\n"
        "@compute(4) fun cs(@local_thread_index li: int){tile}:\n"
        "    tile.values[li] = float(li)\n"
        "    workgroup_barrier()\n"
        "    print \"thread $li: $(tile.values[(li + 1) % 4])\"\n",
    };
    for (auto const source : sources)
    {
        auto const compiler = slib::create_sgl_compiler(
            inner_compiler_for(ctx),
            {.persistent_cache = nullptr, .diagnostics = true, .guards_undefined = slib::sgl_undefined_guard::report});
        auto desc = slib::shader_source_description{.source = cc::string(source),
                                                    .entry_point = "cs",
                                                    .stage = sg::shader_stage::compute,
                                                    .label = "shadow.sgl"};
        slib::add_context_options(desc.options, ctx);
        auto preprocessed = compiler->preprocess(desc, [](cc::string_view) { return cc::optional<cc::string>(); });
        REQUIRE(preprocessed.has_value());
        REQUIRE(preprocessed.value().stated.has_value());
        auto shader = preprocessed.value().stated.value();

        desc.source = preprocessed.value().source;
        desc.bytecode = cc::move(preprocessed.value().bytecode);
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
        co_await ctx.epochs_in_flight_completion(2);
        ctx.advance_epoch();

        co_await ctx.idle_completion();
        co_await ctx.idle_completion();
        ctx.process_completed_epochs();
        co_await ctx.idle_completion();
    }
}


ASYNC_INVOCABLE_TEST("sr - an index past an element of a binding array is measured against that element: reported, or "
                     "under define the element's first (LEGAL-66, LEGAL-68)",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    if (inner_compiler_for(ctx) == nullptr || !ctx.supports(sg::feature::binding_arrays))
        SKIP("this backend has no binding arrays, or sg binds no debug window on it yet");

    // thread 2 reads element 3 of a view of 2; thread 3 reads element 4 of a view of 8, which is fine
    constexpr auto source = "require binding_arrays\n"
                            "\n"
                            "binding work:\n"
                            "    written: mut buffer[uint]\n"
                            "    views: buffer[uint][2]\n"
                            "\n"
                            "@compute(4) fun cs(@thread_id id: int3){work}:\n"
                            "    work.written[id.x] = work.views[nonuniform (id.x % 2)][id.x + 1]\n";
    nx::expect_error("views.sgl:8: assert failed: index 3 is outside a buffer of 2 elements", nx::exactly(1));

    auto const short_view
        = ctx.persistent.create_buffer_from_data(cc::vector<u32>{10, 11}, sg::buffer_usage::readonly_buffer);
    auto const long_view = ctx.persistent.create_buffer_from_data(cc::vector<u32>{20, 21, 22, 23, 24, 25, 26, 27},
                                                                  sg::buffer_usage::readonly_buffer);
    struct run
    {
        slib::sgl_undefined_guard guard = slib::sgl_undefined_guard::report;
        u32 third = 0;
    };
    // report ends the thread before it writes, and define reads the view's first element
    run const runs[] = {{.guard = slib::sgl_undefined_guard::report, .third = 99},
                        {.guard = slib::sgl_undefined_guard::define, .third = 10}};
    for (auto const& r : runs)
    {
        auto const written = ctx.persistent.create_buffer_from_data(
            cc::vector<u32>{99, 99, 99, 99}, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
        auto views = cc::vector<sg::raw_view>();
        views.push_back(short_view.as_readonly_buffer());
        views.push_back(long_view.as_readonly_buffer());
        auto const compute = co_await guarded_compute_of(ctx, source, "views.sgl", r.guard, written, cc::move(views));

        auto cmd = ctx.create_command_list();
        cmd->compute.bind_pipeline(*compute.pipeline);
        cmd->compute.bind_group(0, *compute.group);
        auto const reads = cc::vector<sg::array_buffer_access>{{.index = 0, .access = sg::access_flag::shader_read},
                                                               {.index = 1, .access = sg::access_flag::shader_read}};
        auto const declared = cmd->compute.declare_array_buffer_access("work.views", reads);
        cmd->compute.dispatch_threads(4);
        auto const future = cmd->download.data_from_buffer(written);
        ctx.submit_command_list(cc::move(cmd));
        co_await ctx.epochs_in_flight_completion(2);
        ctx.advance_epoch();

        auto const data = co_await future.data();
        REQUIRE(data.size() == 4);
        CHECK(data[0] == 11);
        CHECK(data[1] == 22);
        CHECK(data[2] == r.third);
        CHECK(data[3] == 24);

        co_await ctx.idle_completion();
        co_await ctx.idle_completion();
        ctx.process_completed_epochs();
        co_await ctx.idle_completion();
    }
}

ASYNC_INVOCABLE_TEST("sr - the length of a binding array's element at a non-uniform index is that element's, in a "
                     "fixed array and an unbounded one",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    if (inner_compiler_for(ctx) == nullptr || !ctx.supports(sg::feature::binding_arrays))
        SKIP("this backend has no binding arrays, or sg binds no debug window on it yet");

    // AMD's vulkan driver once answered every lane with element 0's length (debt-sgl-vulkan-element-length-asked-at-uniform-indices)
    struct array_kind
    {
        cc::string_view require;
        cc::string_view extent;
    };
    auto kinds = cc::vector<array_kind>{{.require = "binding_arrays", .extent = "2"}};
    if (ctx.supports(sg::feature::unbounded_binding_arrays))
        kinds.push_back({.require = "binding_arrays, unbounded_binding_arrays", .extent = ""});
    auto const short_view
        = ctx.persistent.create_buffer_from_data(cc::vector<u32>{10, 11}, sg::buffer_usage::readonly_buffer);
    auto const long_view = ctx.persistent.create_buffer_from_data(cc::vector<u32>{20, 21, 22, 23, 24, 25, 26, 27},
                                                                  sg::buffer_usage::readonly_buffer);
    for (auto const& kind : kinds)
    {
        auto const source
            = cc::format("require {}\n\nbinding work:\n    written: mut buffer[uint]\n    views: buffer[uint][{}]\n"
                         "\n@compute(4) fun cs(@thread_id id: int3){{work}}:\n"
                         "    work.written[id.x] = work.views[nonuniform (id.x % 2)].length\n",
                         kind.require, kind.extent);
        auto const written = ctx.persistent.create_buffer_from_data(
            cc::vector<u32>{99, 99, 99, 99}, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
        auto views = cc::vector<sg::raw_view>();
        views.push_back(short_view.as_readonly_buffer());
        views.push_back(long_view.as_readonly_buffer());
        auto const compute = co_await guarded_compute_of(ctx, source, "length.sgl", slib::sgl_undefined_guard::none,
                                                         written, cc::move(views));

        auto cmd = ctx.create_command_list();
        cmd->compute.bind_pipeline(*compute.pipeline);
        cmd->compute.bind_group(0, *compute.group);
        auto const reads = cc::vector<sg::array_buffer_access>{{.index = 0, .access = sg::access_flag::shader_read},
                                                               {.index = 1, .access = sg::access_flag::shader_read}};
        auto const declared = cmd->compute.declare_array_buffer_access("work.views", reads);
        cmd->compute.dispatch_threads(4);
        auto const future = cmd->download.data_from_buffer(written);
        ctx.submit_command_list(cc::move(cmd));
        co_await ctx.epochs_in_flight_completion(2);
        ctx.advance_epoch();

        auto const data = co_await future.data();
        REQUIRE(data.size() == 4);
        CHECK(data[0] == 2);
        CHECK(data[1] == 8);
        CHECK(data[2] == 2);
        CHECK(data[3] == 8);

        co_await ctx.idle_completion();
        co_await ctx.idle_completion();
    }
}

ASYNC_INVOCABLE_TEST("sr - define runs a load of more words than the memory holds on none of them, and gives zero "
                     "(LEGAL-68)",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    if (inner_compiler_for(ctx) == nullptr || !ctx.supports(sg::feature::binding_arrays))
        SKIP("this backend has no binding arrays, or sg binds no debug window on it yet");

    // the even threads load two words of a view of one
    constexpr auto source = "require binding_arrays\n"
                            "\n"
                            "binding work:\n"
                            "    written: mut buffer[uint]\n"
                            "    raw: bytes[2]\n"
                            "\n"
                            "@compute(4) fun cs(@thread_id id: int3){work}:\n"
                            "    let words = work.raw[nonuniform (id.x % 2)].load2(0u)\n"
                            "    work.written[id.x] = words.x + words.y\n";

    auto const written = ctx.persistent.create_buffer_from_data(
        cc::vector<u32>{99, 99, 99, 99}, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const one_word = ctx.persistent.create_buffer_from_data(cc::vector<u32>{7}, sg::buffer_usage::readonly_buffer);
    auto const two_words
        = ctx.persistent.create_buffer_from_data(cc::vector<u32>{20, 22}, sg::buffer_usage::readonly_buffer);
    auto views = cc::vector<sg::raw_view>();
    views.push_back(sg::buffer<byte>::from_raw(one_word.raw()).as_readonly_buffer());
    views.push_back(sg::buffer<byte>::from_raw(two_words.raw()).as_readonly_buffer());
    auto const compute = co_await guarded_compute_of(ctx, source, "short.sgl", slib::sgl_undefined_guard::define,
                                                     written, cc::move(views));

    auto cmd = ctx.create_command_list();
    cmd->compute.bind_pipeline(*compute.pipeline);
    cmd->compute.bind_group(0, *compute.group);
    auto const reads = cc::vector<sg::array_buffer_access>{{.index = 0, .access = sg::access_flag::shader_read},
                                                           {.index = 1, .access = sg::access_flag::shader_read}};
    auto const declared = cmd->compute.declare_array_buffer_access("work.raw", reads);
    cmd->compute.dispatch_threads(4);
    auto const future = cmd->download.data_from_buffer(written);
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const data = co_await future.data();
    REQUIRE(data.size() == 4);
    CHECK(data[0] == 0);
    CHECK(data[1] == 42);
    CHECK(data[2] == 0);
    CHECK(data[3] == 42);

    co_await ctx.idle_completion();
    co_await ctx.idle_completion();
    ctx.process_completed_epochs();
    co_await ctx.idle_completion();
}

ASYNC_INVOCABLE_TEST("sr - a mesh stage's read of workgroup memory no invocation stored is reported where it would "
                     "have "
                     "run (LEGAL-69)",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    if (inner_compiler_for(ctx) == nullptr)
        SKIP("sg binds no debug window on this backend yet");
    if (!ctx.supports(sg::feature::mesh_shader))
        SKIP("this context draws no mesh stage");
    if (auto const& adapter = ctx.metrics.adapter(); adapter.vendor_id == 0x10de && !adapter.is_software)
        SKIP("NVIDIA's driver loses the shadow marks of invocations 1 and 3; see "
             "docs/bugs-external/nvidia-mesh-groupshared-atomics-lost");
    // invocation 2 stores nothing, so invocation 1 reads a hole, after the barrier that would have shown it a store
    nx::expect_error("mesh_shadow.sgl:13: assert failed: workgroup memory `tile.values[2]` is read before any "
                     "invocation stored it",
                     nx::exactly(1));

    auto const source = cc::string("require mesh_shader\n"
                                   "struct corner:\n"
                                   "    @position position: hpos4\n"
                                   "@pixel struct shaded:\n"
                                   "    color: float4\n"
                                   "@workgroup binding tile:\n"
                                   "    values: float[4]\n"
                                   "@mesh(4, vertices = 4, triangles = 2)\n"
                                   "fun ms(@local_thread_index k: int, output: mut mesh_output[corner]){tile}:\n"
                                   "    if k != 2:\n"
                                   "        tile.values[k] = float(k)\n"
                                   "    workgroup_barrier()\n"
                                   "    let v = tile.values[(k + 1) % 4]\n"
                                   "    output.set_counts(4u, 2u)\n"
                                   "    let x = ((k & 1) as float) * 2.0 - 1.0 + v * 0.0\n"
                                   "    let y = ((k >> 1) as float) * 2.0 - 1.0\n"
                                   "    output.vertex(k as uint, {position = hpos4(x, y, 0.0, 1.0)})\n"
                                   "    if k == 0:\n"
                                   "        output.triangle(0u, uint3(0u, 1u, 2u))\n"
                                   "        output.triangle(1u, uint3(1u, 3u, 2u))\n"
                                   "@pixel fun ps(c: corner) -> shaded:\n"
                                   "    return {color = float4(1.0)}\n");
    auto const guard = slib::sgl_undefined_guard::report;
    auto const mesh = co_await guarded_stage_of(ctx, source, "ms", sg::shader_stage::mesh, "mesh_shadow.sgl", guard);
    auto const pixel = co_await guarded_stage_of(ctx, source, "ps", sg::shader_stage::fragment, "mesh_shadow.sgl", guard);

    auto desc = sg::raster_pipeline_description{
        .layout = ctx.uncached.create_pipeline_layout({}),
        .fragment_shader = pixel,
        .mesh_shader = mesh,
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});
    auto const pipeline = ctx.uncached.create_raster_pipeline(desc);
    auto const target = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 4, .height = 4, .usage = sg::texture_usage::render_target});

    auto cmd = ctx.create_command_list();
    {
        auto pass = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0))}});
        pass.bind_pipeline(*pipeline);
        pass.draw_mesh_tasks(1);
    }
    ctx.submit_command_list(cc::move(cmd));
    co_await ctx.epochs_in_flight_completion(2);
    ctx.advance_epoch();

    co_await ctx.idle_completion();
    co_await ctx.idle_completion();
    ctx.process_completed_epochs();
    co_await ctx.idle_completion();
}
