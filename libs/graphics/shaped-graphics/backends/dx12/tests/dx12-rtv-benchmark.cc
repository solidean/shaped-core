#include "dx12-test-common.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_cpu_descriptor_heap.hh>
#include <shaped-graphics/backends/dx12/dx12_epoch.hh>
#include <shaped-graphics/backends/dx12/dx12_view_desc.hh>

// What a rendering scope's RTVs and DSV cost under two ownership models.
//
// - heap slot per scope: the model dx12_command_list's own slots replaced.
//   Every target allocates a slot from the context's RTV or DSV heap under its mutex and writes the descriptor.
//   end_rendering stages one finalizer per slot, and the slots are freed when the epoch retires.
// - list slots: dx12_command_list's model, which owns `max_color_targets` RTV slots and one DSV slot for its life.
//   Every scope writes its descriptors into them, overwriting the previous scope's, and nothing is freed or deferred.
//   D3D12 reads a CPU descriptor when the call that takes it records, so the overwrite is safe —
//   tests/backends/dx12-rtv-record-time-test.cc in shaped-graphics-test pins that on the device.
//
// Both models run the real CreateRenderTargetView / CreateDepthStencilView into the real heaps.
// The epoch retire is emulated as a ring two epochs deep, so the finalizers run inside the measurement exactly as a
// frame loop pays them; recording and the GPU are absent, since they are the same in both models.
//
// The debug layer is process-wide and one-way, so the two benchmarks below cannot share a process.
// Run each by its exact name.

using namespace cc::primitive_defines;

namespace
{
namespace dx12 = sg::backend::dx12;

constexpr int k_scopes_per_frame = 4;
constexpr int k_epochs_in_flight = 2;
constexpr int k_epoch_ring = k_epochs_in_flight + 1;

struct frame_targets
{
    cc::vector<sg::texture_2d> colors;
    sg::texture_2d depth;
};

[[nodiscard]] frame_targets make_targets(dx12::dx12_context& ctx, int color_count)
{
    auto targets = frame_targets();
    for (auto i = 0; i < color_count; ++i)
        targets.colors.push_back(ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                                   .width = 256,
                                                                   .height = 256,
                                                                   .usage = sg::texture_usage::render_target}));
    targets.depth = ctx.persistent.create_texture_2d({.format = sg::pixel_format::depth32_float,
                                                      .width = 256,
                                                      .height = 256,
                                                      .usage = sg::texture_usage::depth_stencil});
    return targets;
}

/// The replaced model: a heap slot per target per scope, freed by a finalizer when the scope's epoch retires.
class heap_slot_model
{
public:
    explicit heap_slot_model(dx12::dx12_context& ctx) : _ctx(ctx) {}

    void begin_frame()
    {
        ++_frame;
        auto const slot = int(_frame % u64(k_epoch_ring));
        _staged.lock(
            [&](rings& r)
            {
                for (auto& expiring : r.epochs[slot])
                    for (auto& f : expiring.finalizers)
                        f();
                r.epochs[slot].clear();
            });
    }

    void scope(frame_targets const& targets)
    {
        cc::fixed_vector<dx12::cpu_descriptor_slot, sg::max_color_targets> rtv_slots;
        for (auto const& color : targets.colors)
        {
            auto const rtv = _ctx.create_dx12_render_target_view(color.as_render_target_view());
            CC_ASSERT(rtv.has_value(), "RTV heap exhausted");
            nx::bench::sink(rtv.value().handle.ptr);
            rtv_slots.push_back(rtv.value().slot);
        }
        auto const dsv = _ctx.create_dx12_depth_stencil_view(targets.depth.as_depth_stencil_view());
        CC_ASSERT(dsv.has_value(), "DSV heap exhausted");
        nx::bench::sink(dsv.value().handle.ptr);

        // raster_end_rendering's staging, verbatim but for where the expiring resource goes.
        auto expiring = dx12::dx12_expiring_resource();
        for (auto const slot : rtv_slots)
            expiring.finalizers.push_back([ctx = &_ctx, slot]() { ctx->free_dx12_render_target_view(slot); });
        expiring.finalizers.push_back([ctx = &_ctx, slot = dsv.value().slot]()
                                      { ctx->free_dx12_depth_stencil_view(slot); });
        auto const ring_slot = int(_frame % u64(k_epoch_ring));
        _staged.lock([&](rings& r) { r.epochs[ring_slot].push_back(cc::move(expiring)); });
    }

    void drain()
    {
        for (auto i = 0; i < k_epoch_ring; ++i)
            begin_frame();
    }

private:
    struct rings
    {
        cc::vector<dx12::dx12_expiring_resource> epochs[k_epoch_ring];
    };

    dx12::dx12_context& _ctx;
    u64 _frame = 0;
    cc::mutex<rings> _staged;
};

/// dx12_command_list's model: slots owned by the command list, overwritten by every scope.
class list_slot_model
{
public:
    explicit list_slot_model(dx12::dx12_context& ctx) : _ctx(ctx)
    {
        for (auto i = 0; i < sg::max_color_targets; ++i)
            _rtv_slots.push_back(_ctx._rtv_heap->allocate());
        _dsv_slot = _ctx._dsv_heap->allocate();
    }

    ~list_slot_model()
    {
        for (auto const slot : _rtv_slots)
            _ctx.free_dx12_render_target_view(slot);
        _ctx.free_dx12_depth_stencil_view(_dsv_slot);
    }

    list_slot_model(list_slot_model const&) = delete;
    list_slot_model& operator=(list_slot_model const&) = delete;

    void begin_frame() {}

    void scope(frame_targets const& targets)
    {
        auto* const device = _ctx._device.Get();
        for (auto i = 0; i < int(targets.colors.size()); ++i)
        {
            auto const handle = _ctx._rtv_heap->cpu_at(_rtv_slots[i]);
            dx12::create_render_target_view(device, targets.colors[i].as_render_target_view(), handle);
            nx::bench::sink(handle.ptr);
        }
        auto const handle = _ctx._dsv_heap->cpu_at(_dsv_slot);
        dx12::create_depth_stencil_view(device, targets.depth.as_depth_stencil_view(), handle);
        nx::bench::sink(handle.ptr);
    }

    void drain() {}

private:
    dx12::dx12_context& _ctx;
    cc::fixed_vector<dx12::cpu_descriptor_slot, sg::max_color_targets> _rtv_slots;
    dx12::cpu_descriptor_slot _dsv_slot = dx12::cpu_descriptor_slot::invalid;
};

template <class Model>
void measure(cc::string_view name, Model& model, frame_targets const& targets)
{
    auto const views_per_frame = k_scopes_per_frame * (int(targets.colors.size()) + 1);
    nx::bench::run(name, {.min_time_secs = 0.2},
                   [&](nx::bench::iteration& it)
                   {
                       model.begin_frame();
                       for (auto s = 0; s < k_scopes_per_frame; ++s)
                           model.scope(targets);
                       it.items(views_per_frame);
                   });
    model.drain();
}

void run_models(dx12::dx12_context& ctx, char const* mode)
{
    auto const& adapter = ctx.metrics.adapter().name;
    for (auto const color_count : {1, sg::max_color_targets})
    {
        auto const targets = make_targets(ctx, color_count);
        {
            auto model = heap_slot_model(ctx);
            measure(cc::format("{} - {}, {} color + depth - heap slot per scope", adapter, mode, color_count), model,
                    targets);
        }
        {
            auto model = list_slot_model(ctx);
            measure(cc::format("{} - {}, {} color + depth - list slots", adapter, mode, color_count), model, targets);
        }
    }
}

/// Whether the D3D12 debug layer validates `ctx`'s device — which a device only does if the layer was on when it was created.
[[nodiscard]] bool is_validated(dx12::dx12_context& ctx)
{
    auto debug_device = dx12::ComPtr<ID3D12DebugDevice>();
    return SUCCEEDED(ctx._device.As(&debug_device));
}
} // namespace

BENCHMARK("sg dx12 - render target views - no debug layer")
{
    auto created = dx12::as_test_context(sg::create_dx12_context({}));
    if (created.has_error())
        SKIP("no dx12 device");
    auto& ctx = *created.value();
    if (is_validated(ctx))
        SKIP("the debug layer is already active in this process; run this benchmark by its exact name");
    run_models(ctx, "no debug layer");
}

BENCHMARK("sg dx12 - render target views - debug layer")
{
    auto created = dx12::as_test_context(sg::create_dx12_context({.activate_global_debug_layer = true}));
    if (created.has_error())
        SKIP("no dx12 device with the debug layer; run this benchmark by its exact name, before any other device");
    auto& ctx = *created.value();
    if (!is_validated(ctx))
        SKIP("the debug layer is not installed");
    run_models(ctx, "debug layer");
}
