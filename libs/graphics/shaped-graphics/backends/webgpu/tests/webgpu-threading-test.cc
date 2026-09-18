#include "webgpu-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/logs.hh>
#include <shaped-graphics/all.hh>

// The free-threaded surface of a main_thread context, used from a pool worker: asyncs, layouts, and dropping what it made.
// A caller there — any helper coroutine a main-homed body awaits, in a threaded wasm build — would otherwise touch a
// WebGPU realm its callbacks never reach, and hang rather than fail.
// Without threads the pool runs on the calling thread, so this passes trivially there and means something only under `-pthread`.

NX_ALLOW_LOGS(cc::rec::level::warning, "bcache", "cache degraded to always-miss");

namespace
{
namespace webgpu = sg::backend::webgpu;
using webgpu::test::make_shader;

constexpr char const* k_fill = R"(
@group(0) @binding(0) var<storage, read_write> Output: array<u32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3u) {
    if (id.x < arrayLength(&Output)) {
        Output[id.x] = 7u;
    }
}
)";
} // namespace

ASYNC_TEST("sg webgpu - the free-threaded surface works from a pool worker", main_thread)
{
    co_await cc::async_resume_on_compute();
    auto const requested = co_await cc::async_as_result(sg::request_webgpu_context());
    if (requested.has_error())
    {
        SKIP("no webgpu adapter");
        co_return;
    }
    auto const ctx = std::static_pointer_cast<webgpu::webgpu_context>(requested.value());
    CHECK(ctx->threading() == sg::thread_model::main_thread);

    // Layouts describe; their WebGPU objects are made on first use, which the build below makes on main.
    auto const shader = make_shader(sg::shader_stage::compute, k_fill, "main",
                                    {sg::binding{.name = "Output",
                                                 .group_index = 0,
                                                 .index = 0,
                                                 .count = 1,
                                                 .type = sg::binding_type::readwrite_structured_buffer}},
                                    sg::compute_dimensions{.x = 64});
    auto group_layout = ctx->cached.acquire_binding_group_layout(shader.bindings);
    auto pipeline_layout = ctx->cached.acquire_pipeline_layout({.groups = {group_layout}});
    auto pipeline = co_await ctx->uncached.create_compute_pipeline_async({.shader = shader, .layout = pipeline_layout});
    CHECK(pipeline != nullptr);

    // Resource creation is bound, so the buffer is made on main; dropping it back here must not release it here.
    co_await cc::async_resume_on_main();
    auto buffer = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer);
    co_await cc::async_resume_on_compute();
    buffer = nullptr;
    pipeline = nullptr;
    pipeline_layout = nullptr;
    group_layout = nullptr;

    // The drops above release on main, at the next advance.
    co_await cc::async_resume_on_main();
    ctx->advance_epoch();
    co_await cc::async_settled(ctx->idle_completion());
    CHECK(ctx->take_pending_errors().empty());
    ctx->shutdown();
}
