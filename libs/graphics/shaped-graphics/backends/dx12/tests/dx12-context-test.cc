#include "dx12-test-common.hh"

#include <clean-core/platform/environment.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_runtime.hh>

using namespace cc::primitive_defines;

// dx12 backend bring-up: a command list and buffers through the public sg::context API.
// Part of the dx12 test binary (shaped-graphics-dx12-test), built only where the dx12 backend builds (Windows), so it never needs an #ifdef guard.
// See libs/graphics/shaped-graphics/docs/concepts/backends.md for how backend tests are organized.
//
// Both adapters are covered because the entry drivers (dx12-entry.cc) run every invocable on each: WARP, always present, and the real GPU when there is one.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

INVOCABLE_TEST("sg dx12 - context brings up lists and buffers", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    CHECK(handle->backend() == sg::backend_kind::dx12);
    CHECK(handle->threading() == sg::thread_model::multi_threaded);

    auto& ctx = *handle;

    // Command list: handed out already recording.
    // Submitting consumes it, since it is moved in.
    auto cmd = ctx.create_command_list();
    REQUIRE(cmd != nullptr);
    ctx.submit_command_list(cc::move(cmd));

    // Buffer with real GPU storage.
    auto buf = ctx.persistent.create_raw_buffer(256, sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);
    CHECK(buf->size_in_bytes() == 256);
    CHECK(buf->usage().has(sg::buffer_usage::copy_dst));

    // Empty buffer: valid and zero-length (the "no backing GPU resource" invariant is pinned separately below).
    auto empty = ctx.persistent.create_raw_buffer(0, {});
    REQUIRE(empty != nullptr);
    CHECK(empty->size_in_bytes() == 0);

    // read-write storage (UAV) usage takes the storage-buffer allocation path.
    auto storage = ctx.persistent.create_raw_buffer(1024, sg::buffer_usage::readwrite_buffer);
    REQUIRE(storage != nullptr);
    CHECK(storage->size_in_bytes() == 1024);

    // Explicit drop also consumes the list.
    auto to_drop = ctx.create_command_list();
    REQUIRE(to_drop != nullptr);
    ctx.drop_command_list(cc::move(to_drop));

    // The same paths once more: create a buffer, and create + drop a command list.
    auto via_base = ctx.persistent.create_raw_buffer(64, sg::buffer_usage::vertex_buffer);
    REQUIRE(via_base != nullptr);
    CHECK(via_base->size_in_bytes() == 64);

    auto base_cmd = ctx.create_command_list();
    REQUIRE(base_cmd != nullptr);
    ctx.drop_command_list(cc::move(base_cmd));
}

// Backend-internal invariant, with no public equivalent — hence the downcast to the concrete buffer.
// A size-0 buffer holds no ID3D12Resource, while a non-empty one does.
INVOCABLE_TEST("sg dx12 - a zero-size buffer allocates no backing resource", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.create_dx12_buffer(256, sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());
    CHECK(buf.value()->_resource != nullptr); // real storage -> a backing resource

    auto empty = c.create_dx12_buffer(0, {}, sg::allocation_info{});
    REQUIRE(empty.has_value());
    CHECK(empty.value()->_resource == nullptr); // size 0 -> no resource allocated
}

// The variable is process-wide, so this runs beside nothing: a driver creating its context meanwhile would lose its GPU.
TEST("sg dx12 - SC_DX12_ADAPTER=warp hides the hardware adapter from every request", exclusive())
{
    auto const pin = cc::scoped_environment_variable("SC_DX12_ADAPTER", "warp");

    CHECK(!dx12::has_hardware_adapter());

    // Both ask for the debug layer, like every other context in this binary.
    // The process's first device decides whether the layer can ever be activated, and this test may run first.
    // An explicit hardware request is refused too, and the error names the pin as the reason.
    auto const hardware
        = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::hardware});
    REQUIRE(hardware.has_error());
    CHECK(hardware.error().to_string().contains("SC_DX12_ADAPTER"));

    auto const fallback
        = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::hardware_or_warp});
    REQUIRE(fallback.has_value());
    CHECK(fallback.value()->metrics.adapter().is_software);
}

// The mirror, and what keeps WARP off every CI job but one: a WARP driver skips when its context cannot be created.
TEST("sg dx12 - SC_DX12_ADAPTER=hardware hides WARP from every request", exclusive())
{
    auto const pin = cc::scoped_environment_variable("SC_DX12_ADAPTER", "hardware");

    auto const warp = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::warp});
    REQUIRE(warp.has_error());
    CHECK(warp.error().to_string().contains("SC_DX12_ADAPTER"));

    // Where there is a GPU the fallback takes it; where there is none it has nothing left to fall back to.
    auto const fallback
        = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::hardware_or_warp});
    if (dx12::has_hardware_adapter())
    {
        REQUIRE(fallback.has_value());
        CHECK(!fallback.value()->metrics.adapter().is_software);
    }
    else
        CHECK(fallback.has_error());
}

// What an XR runtime names a GPU by, and the only way to put two APIs on one adapter.
TEST("sg dx12 - a context created by LUID runs on that adapter, whatever `adapter` asks")
{
    auto const warp = dx12::as_test_context(
        sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::warp}));
    if (warp.has_error())
        SKIP("no WARP adapter");
    REQUIRE(warp.value()->metrics.adapter().luid.has_value());
    auto const luid = warp.value()->metrics.adapter().luid.value();

    // make_test_context asks for hardware_or_warp, which the LUID overrides even where a GPU is there to take.
    auto const by_luid = dx12::make_test_context({.adapter_luid = luid, .minimum_feature_level = D3D_FEATURE_LEVEL_12_0});
    REQUIRE(by_luid.has_value());
    CHECK(by_luid.value()->metrics.adapter().is_software);
    CHECK(by_luid.value()->metrics.adapter().luid == luid);
    CHECK(by_luid.value()->device() != nullptr);
    CHECK(by_luid.value()->queue() != nullptr);
}

TEST("sg dx12 - a LUID no adapter has fails creation rather than falling back")
{
    auto const ctx = dx12::make_test_context({.adapter_luid = ~u64(0)});
    REQUIRE(ctx.has_error());
    CHECK(ctx.error().to_string().contains("LUID"));
}

// The variable is process-wide, so this runs beside nothing, like the two above it.
TEST("sg dx12 - SC_DX12_ADAPTER hides the adapter a LUID names", exclusive())
{
    auto const warp = dx12::as_test_context(
        sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::warp}));
    if (warp.has_error())
        SKIP("no WARP adapter");
    auto const luid = warp.value()->metrics.adapter().luid.value();

    auto const pin = cc::scoped_environment_variable("SC_DX12_ADAPTER", "hardware");
    auto const hidden = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter_luid = luid});
    REQUIRE(hidden.has_error());
    CHECK(hidden.error().to_string().contains("SC_DX12_ADAPTER"));
}

// The variable is process-wide, so this runs beside nothing; on a machine with two GPUs it reaches both.
TEST("sg dx12 - SC_GPU puts a context on the vendor or the name it gives, and fails where none matches", exclusive())
{
    if (!dx12::has_hardware_adapter())
        SKIP("no hardware adapter");

    struct vendor
    {
        char const* name;
        u32 id;
    };
    auto found = 0;
    for (auto const v : {vendor{.name = "intel", .id = 0x8086}, vendor{.name = "amd", .id = 0x1002},
                         vendor{.name = "nvidia", .id = 0x10DE}})
    {
        auto const pin = cc::scoped_environment_variable("SC_GPU", v.name);
        auto const ctx
            = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::hardware});
        if (ctx.has_error())
        {
            CHECK(ctx.error().to_string().contains("SC_GPU"));
            continue;
        }
        ++found;
        CHECK(ctx.value()->metrics.adapter().vendor_id == v.id);

        // The adapter's own name reaches it as well.
        auto const name = ctx.value()->metrics.adapter().name;
        auto const by_name = cc::scoped_environment_variable("SC_GPU", name);
        auto const again
            = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::hardware});
        REQUIRE(again.has_value());
        CHECK(again.value()->metrics.adapter().name == name);
    }
    if (found == 0)
        SKIP("no Intel, AMD or NVIDIA adapter");

    // WARP is never taken in a missing GPU's place, and an adapter named in code still wins.
    auto const pin = cc::scoped_environment_variable("SC_GPU", "no such gpu");
    for (auto const adapter : {dx12::dx12_adapter::hardware, dx12::dx12_adapter::hardware_or_warp})
    {
        auto const ctx = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = adapter});
        REQUIRE(ctx.has_error());
        CHECK(ctx.error().to_string().contains("SC_GPU='no such gpu'"));
    }
    auto const warp = sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::warp});
    if (warp.has_value())
    {
        auto const luid = warp.value()->metrics.adapter().luid.value();
        CHECK(sg::create_dx12_context({.activate_global_debug_layer = true, .adapter_luid = luid}).has_value());
    }
}

// A context that withholds ray tracing answers as one on an adapter without DXR, on every spelling of the question.
TEST("sg dx12 - a context withholding ray tracing reports neither half of it")
{
    auto const ctx = dx12::make_test_context({.withhold_raytracing = true});
    if (ctx.has_error())
        SKIP("no dx12 adapter");
    auto& c = *ctx.value();
    CHECK(!c.supports(sg::feature::ray_query));
    CHECK(!c.supports(sg::feature::raytracing_pipeline));
    CHECK(!c.supports_raytracing());

    auto probe = c.create_command_list();
    CHECK(!probe->raytracing.is_supported());
    c.drop_command_list(cc::move(probe));
}

// The count is what the context's destructor asserts is zero, since a resource outliving it releases into freed memory.
ASYNC_INVOCABLE_TEST("sg dx12 - a context counts the textures and buffers alive against it",
                     (dx12::dx12_context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    // The invocables run in sequence, so once the earlier ones' work has drained no other resource moves the count.
    ctx->advance_epoch();
    co_await ctx->idle_completion();
    auto const before = ctx->live_resource_count();
    {
        auto const buffer = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::copy_dst);
        auto const texture = ctx->persistent.create_texture_2d(
            {.format = sg::pixel_format::rgba8_unorm, .width = 4, .height = 4, .usage = sg::texture_usage::copy_dst});
        CHECK(ctx->live_resource_count() == before + 2);
    }
    CHECK(ctx->live_resource_count() == before);
}

TEST("sg dx12 - devices come from the Agility SDK exactly where it was fetched and asked for")
{
    // A fetched SDK that fails to load would quietly cost every SM 6.9 feature, so a process asking for it asserts it loaded.
    auto const asked = SG_HAS_AGILITY && dx12::requested_d3d12_runtime() == dx12::d3d12_runtime::agility;
    CHECK(dx12::runs_agility_runtime() == asked);
    if (!SG_HAS_AGILITY)
        SKIP("the Agility SDK was not fetched (uv run extern/agility/fetch-agility.py --accept-license)");
    if (!asked)
        SKIP("this process runs the system runtime by choice; sg's test binary loads the Agility SDK in a process of "
             "its own");
    dx12::ComPtr<ID3D12Device> device;
    REQUIRE(SUCCEEDED(dx12::create_d3d12_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));
    // the runtime that ships Shader Model 6.9 knows to answer for it, whatever the adapter's driver then supports
    auto shader_model = D3D12_FEATURE_DATA_SHADER_MODEL{.HighestShaderModel = D3D_SHADER_MODEL_6_9};
    CHECK(SUCCEEDED(
        dx12::check_feature_support(device.Get(), D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model))));
}

// The one cap of DXR's preview operations, read only through the experimental features the preview runtime opts into.
// libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md is what sg builds on it.
INVOCABLE_TEST("sg dx12 - only the preview runtime reports clustered geometry and partitioned TLAS",
               (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    // a retail build never enables the preview, whatever runtime it found
    CHECK((SG_HAS_AGILITY_PREVIEW || !dx12::runs_raytracing_preview()));
    if (!dx12::runs_raytracing_preview())
    {
        CHECK(!handle->supports_clusters_and_ptlas());
        if (!SG_HAS_AGILITY_PREVIEW)
            SKIP("not built against the preview Agility SDK (uv run dev.py test --agility-preview)");
        if (!dx12::runs_agility_runtime())
            SKIP("the preview Agility SDK did not load, which it does only with Windows Developer Mode on");
        SKIP("the preview Agility SDK refused its experimental features");
    }
    if (!handle->supports_clusters_and_ptlas())
        SKIP("this adapter's driver does not report ClustersAndPTLASSupported");
    // the cap promises Shader Model 6.10, and with it the inline traces a partitioned TLAS is read through
    CHECK(handle->supports_ray_query());
}
