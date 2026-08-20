#include "dx12-test-common.hh"

#include <blob-cache/blob_cache.hh>
#include <clean-core/common/utility.hh> // cc::memcmp
#include <clean-core/container/pinned_data.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// Embedded DXIL for double_compute.hlsl (Output[i] = i*2). See dx12-compute-test.cc.
#include "double_compute.dxil.h"

using namespace cc::primitive_defines;

// Exercises the optional cached-PSO path on WARP.
// A pipeline's serialized blob (cached_pipeline_data) can seed a second pipeline's creation, via compute_pipeline_description::cached_pipeline.
// The seeded pipeline still dispatches correctly, a garbage blob degrades to a fresh build, and the blob is not part of the built-in cache key.

namespace
{
namespace dx12 = sg::backend::dx12;

sg::compiled_shader make_double_shader()
{
    sg::compiled_shader shader;
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::dxil;
    shader.entry_point = "main";
    shader.workgroup_size = sg::compute_dimensions{.x = 64, .y = 1, .z = 1};
    shader.bytecode = cc::make_pinned_data(
        cc::span<byte const>(reinterpret_cast<byte const*>(double_compute_dxil), isize(sizeof(double_compute_dxil))));
    shader.bindings.push_back(sg::binding{
        .name = "Output",
        .set = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    });
    return shader;
}

// Dispatch `pipeline` over `count` threads, reading back Output[i] and checking it equals i*2.
void check_doubles(sg::context& ctx,
                   sg::compute_pipeline const& pipeline,
                   sg::binding_group_layout_handle const& group_layout,
                   int count)
{
    auto buf = ctx.persistent.create_raw_buffer(isize(count) * isize(sizeof(u32)),
                                                sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(buf != nullptr);

    sg::named_view const out = {.name = "Output", .view = sg::buffer<u32>::from_raw(buf).as_readwrite_buffer()};
    auto group = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&out, 1));
    REQUIRE(group != nullptr);

    auto disp = ctx.create_command_list();
    disp->compute.bind_pipeline(pipeline);
    disp->compute.bind_group(0, *group);
    disp->compute.dispatch_threads(count);
    ctx.submit_command_list(cc::move(disp));

    auto down = ctx.create_command_list();
    auto future = down->download.data_from_buffer<u32>(buf, 0, count);
    ctx.submit_command_list(cc::move(down));

    auto const data = ctx.wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == isize(count));
    bool ok = true;
    for (int i = 0; i < count; ++i)
        if (data.value()[i] != u32(i) * 2)
            ok = false;
    CHECK(ok);
}
} // namespace

TEST("sg cached PSO - round-trips a blob and the seeded pipeline still dispatches")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);

    // Build a pipeline from scratch, then read its serialized PSO blob.
    auto first = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(first != nullptr);
    auto const blob = first->cached_pipeline_data();
    CHECK(!blob.empty()); // WARP supports GetCachedBlob

    // Nothing was fed in, so nothing was consumed.
    CHECK(!first->used_cached_pipeline());

    // Seed a second pipeline with that blob and confirm it dispatches identically.
    auto seeded
        = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout, .cached_pipeline = blob});
    REQUIRE(seeded != nullptr);
    CHECK(seeded->used_cached_pipeline()); // the driver accepted its own blob
    check_doubles(ctx, *seeded, group_layout, 256);
}

namespace
{
/// Builds the double-shader pipeline through ctx.cached, with `store` as the persistent tier, and drives it.
///
/// One shape for both presets, which is the point: the build parks on the store either way, and blocking is what
/// resolves it either way.
/// With threads the store answers on its own thread; without them it answers on this one, because a store with no
/// thread registers a pump and cc::async_blocking_get sweeps the registry rather than sleeping.
///
/// No REQUIRE in here: it returns a value, so a failed REQUIRE would have nothing to return.
sg::compute_pipeline_handle build_via_store(sg::context& ctx, sg::compiled_shader const& shader, bcache::blob_cache& store)
{
    ctx.cached.cache().set_blob_cache(&store);

    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    auto const desc = sg::compute_pipeline_description{.shader = shader, .layout = pipeline_layout};

    return cc::async_blocking_get(ctx.cached.acquire_compute_pipeline(desc));
}

/// Feeding a blob back in must keep working after a round trip, whatever the backend did to the bytes.
/// That is the invariant a persistent cache actually rests on: it stores what a pipeline handed it, and a stored blob
/// that would never be accepted again is an entry that can only ever miss.
///
/// It is deliberately NOT a byte comparison.
/// A real dx12 driver re-serializes an accepted blob to DIFFERENT bytes of the same length — measured, not assumed —
/// while WARP reproduces them exactly, so bytes are a backend detail and acceptance is the contract.
void check_blob_round_trips(sg::context& ctx,
                            sg::compiled_shader const& shader,
                            sg::pipeline_layout_handle const& pipeline_layout)
{
    auto first = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(first != nullptr);
    auto const blob = first->cached_pipeline_data();
    REQUIRE(!blob.empty());

    auto seeded
        = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout, .cached_pipeline = blob});
    REQUIRE(seeded != nullptr);
    CHECK(seeded->used_cached_pipeline()).context("a backend rejected a blob it had just produced itself");

    // The second generation must be as good as the first, or refreshing a cache entry would poison it.
    auto const again = seeded->cached_pipeline_data();
    REQUIRE(!again.empty());
    auto third
        = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout, .cached_pipeline = again});
    REQUIRE(third != nullptr);
    CHECK(third->used_cached_pipeline()).context("a re-serialized blob was no longer accepted");
}
} // namespace

TEST("sg cached PSO - a blob survives a round trip through a seeded pipeline")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);

    check_blob_round_trips(ctx, shader, pipeline_layout);
}

TEST("sg cached PSO - a real driver accepts its own blob and rejects a foreign one")
{
    auto handle = dx12::make_hardware_context();
    if (handle == nullptr)
        SKIP("no dx12 hardware adapter");
    sg::context& ctx = *handle;

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);

    auto first = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(first != nullptr);
    CHECK(!first->used_cached_pipeline());

    auto const blob = first->cached_pipeline_data();
    if (blob.empty())
        SKIP("this driver reports no cached PSO blob"); // legal, and it makes the rest meaningless

    // The whole persistent-cache design rests on these two answers coming from a vendor driver rather than from WARP.
    auto seeded
        = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout, .cached_pipeline = blob});
    REQUIRE(seeded != nullptr);
    CHECK(seeded->used_cached_pipeline());
    check_doubles(ctx, *seeded, group_layout, 256);

    // A REAL driver re-serializes to different bytes of the same length, where WARP reproduces them exactly.
    // Measured here, and the reason no cache logic may compare blob bytes to decide anything: doing so would rewrite
    // every entry on every run.
    // used_cached_pipeline() is the signal instead, and what survives a round trip is acceptance, not the bytes.
    check_blob_round_trips(ctx, shader, pipeline_layout);

    {
        // A blob from nowhere must be rejected, not silently accepted — that rejection is the staleness signal.
        dx12::scoped_expected_validation_messages const expect_complaint;

        byte const garbage[64] = {};
        auto rejected = ctx.uncached.try_create_compute_pipeline(
            {.shader = shader,
             .layout = pipeline_layout,
             .cached_pipeline = cc::make_pinned_data(cc::span<byte const>(garbage))});
        REQUIRE(rejected.has_value());
        CHECK(!rejected.value()->used_cached_pipeline());
        check_doubles(ctx, *rejected.value(), group_layout, 256);
    }
}

TEST("sg cached PSO - a persisted blob accelerates a later context")
{
    if (!bcache::blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto probe = dx12::make_hardware_context();
    if (probe == nullptr)
        SKIP("no dx12 hardware adapter");
    probe = nullptr;

    sg::compiled_shader const shader = make_double_shader();

    // A store of this test's own, because this test is ABOUT the store: it has to start empty and stay unshared.
    // Every other test is free to hit the real default cache and be faster for it.
    auto const path = cc::temp_file_path("sg-pso-cache-test", ".db");
    auto store = bcache::blob_cache::create({.path = path});

    // One store, two successive contexts: as close to two runs of the same program as a single test can get.
    // Each context has its own in-memory tier, so the second one genuinely misses in memory and has to reach the store.
    auto first = dx12::make_hardware_context();
    REQUIRE(first != nullptr);
    auto const cold = build_via_store(*first, shader, *store);
    REQUIRE(cold != nullptr);
    CHECK(!cold->used_cached_pipeline()); // nothing to accelerate with yet
    first = nullptr;

    auto second = dx12::make_hardware_context();
    REQUIRE(second != nullptr);
    auto const warm = build_via_store(*second, shader, *store);
    REQUIRE(warm != nullptr);
    CHECK(warm->used_cached_pipeline()).context("the persisted PSO blob did not reach the second context");

    second = nullptr;
    store->close(); // release the file before removing it; SQLite leaves the two siblings beside it
    store = nullptr;
    (void)cc::remove_file(path);
    (void)cc::remove_file(cc::format("{}-wal", path));
    (void)cc::remove_file(cc::format("{}-shm", path));
}

TEST("sg reports which adapter it is running on")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    auto const& adapter = ctx.adapter();

    // WARP is Microsoft's software rasterizer, and identifying it as one is the whole point of the flag: a blob it
    // produced is worth less across machines than a real driver's.
    CHECK(!adapter.name.empty());
    CHECK(adapter.is_software);
    CHECK(adapter.vendor_id != 0);
}

TEST("sg cached PSO - a garbage blob degrades to a fresh build")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    // The unreadable cached blob below is the subject, so the debug layer's complaint about it is the expected outcome rather than a failure.
    dx12::scoped_expected_validation_messages const expect_complaint;

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);

    // A stale/mismatched blob must not hard-fail: creation retries without the cache.
    byte const garbage[64] = {};
    auto res = ctx.uncached.try_create_compute_pipeline(
        {.shader = shader,
         .layout = pipeline_layout,
         .cached_pipeline = cc::make_pinned_data(cc::span<byte const>(garbage))});
    REQUIRE(res.has_value());

    // A rejected blob is exactly what a persistent cache needs to hear: its entry has gone stale.
    CHECK(!res.value()->used_cached_pipeline());
    check_doubles(ctx, *res.value(), group_layout, 256);
}

TEST("sg cached PSO - the blob is not part of the built-in cache key")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    sg::compiled_shader const shader = make_double_shader();
    auto group_layout = ctx.cached.acquire_binding_group_layout(shader.bindings);
    REQUIRE(group_layout != nullptr);
    auto pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    REQUIRE(pipeline_layout != nullptr);

    // Build once to obtain a real blob, then acquire twice: with and without it.
    // Same shader + layout means the same async node, regardless of the accelerator blob.
    auto first = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(first != nullptr);
    auto const blob = first->cached_pipeline_data();

    auto a = ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    auto b = ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = pipeline_layout, .cached_pipeline = blob});
    REQUIRE(a != nullptr);
    CHECK(a.get() == b.get());

    // One node, and a real PSO build on the ambient scheduler — finished here rather than left running past the test.
    (void)cc::try_async_blocking_get(a);
}
