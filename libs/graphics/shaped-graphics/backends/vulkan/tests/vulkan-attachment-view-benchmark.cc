#include "vulkan-test-common.hh"

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// What a rendering scope's attachment VkImageViews cost under three ownership models.
//
// - context map: the model acquire_attachment_view replaced.
//   One map for the whole context, keyed on texture identity plus view fields, under one mutex.
//   An entry dies with its texture, through a finalizer the texture runs at epoch retire.
// - texture epoch slot: acquire_attachment_view itself, called as a rendering scope calls it.
//   Each texture keeps its views stamped with the epoch that created them, under its own mutex.
//   A view stamped with the current epoch is reused; anything else gets a new view.
//   The new view is collected per command list and destroyed when its epoch retires.
// - no cache: a new view on every acquire, destroyed when its epoch retires.
//
// Every model drives the real vkCreateImageView / vkDestroyImageView against real VkImages, with the epoch retire
// emulated as a ring two epochs deep, so the destroys are inside the measurement exactly as a frame loop pays them.
// Recording, submission and the GPU are deliberately absent: they are the same in every model and would only add
// noise to a difference of a few hundred nanoseconds.
//
// A transient target is a new texture every frame.
// It is emulated over the same VkImage — a fresh identity for the context map, an emptied view list for
// acquire_attachment_view — which the driver cannot tell apart from a new image but which keeps vkCreateImage out of
// a number that is about views.
//
// One frame acquires `k_targets` attachments `uses` times each; the per-item time is the cost of one acquire.

using namespace cc::primitive_defines;

namespace
{
namespace vulkan = sg::backend::vulkan;

constexpr int k_color_targets = 8;
constexpr int k_targets = k_color_targets + 1; // the last target is the depth target
constexpr int k_epochs_in_flight = 2;
constexpr int k_epoch_ring = k_epochs_in_flight + 1;

enum class target_lifetime
{
    persistent,
    transient,
};

struct target
{
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    sg::pixel_format sg_format = sg::pixel_format::undefined;
    vulkan::vulkan_texture const* texture = nullptr;
    sg::subresource_range range;
};

[[nodiscard]] VkImageView create_view(VkDevice device, target const& t)
{
    auto const info = VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = t.format,
        .subresourceRange
        = {.aspectMask = t.aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
    };
    VkImageView view = VK_NULL_HANDLE;
    [[maybe_unused]] VkResult const r = vkCreateImageView(device, &info, nullptr, &view);
    CC_ASSERT(r == VK_SUCCESS, "vkCreateImageView failed");
    return view;
}

/// What every model shares: the device, the targets, and which frame is open.
class frame_driver
{
public:
    frame_driver(VkDevice device, cc::span<target const> targets, target_lifetime lifetime)
      : _device(device), _targets(targets), _lifetime(lifetime)
    {
    }

    /// The identity texture `index` has this frame: fixed for a persistent target, fresh every frame for a transient one.
    [[nodiscard]] u64 identity_of(int index) const
    {
        if (_lifetime == target_lifetime::persistent)
            return u64(index + 1);
        return _frame * u64(k_targets) + u64(index + 1);
    }

    [[nodiscard]] int ring_slot() const { return int(_frame % u64(k_epoch_ring)); }

    VkDevice _device = VK_NULL_HANDLE;
    cc::span<target const> _targets;
    target_lifetime _lifetime = target_lifetime::transient;
    u64 _frame = 0;
};

/// The replaced model: one map per context under one mutex, and a per-texture finalizer that evicts the entry.
class context_map_model : public frame_driver
{
public:
    using frame_driver::frame_driver;

    void begin_frame()
    {
        ++_frame;
        // The finalizers of the textures that died three frames ago, run as that epoch retires.
        for (auto& f : _finalizers[ring_slot()])
            f();
        _finalizers[ring_slot()].clear();
    }

    [[nodiscard]] VkImageView acquire(int index)
    {
        auto const& t = _targets[index];
        auto const key = vulkan::vulkan_image_view_key{.texture_identity = identity_of(index), .format = t.sg_format};
        return _views.lock(
            [&](view_map& views) -> VkImageView
            {
                if (auto const* existing = views.get_ptr(key); existing != nullptr)
                    return *existing;

                auto const created = create_view(_device, t);
                views[key] = created;
                // A persistent texture outlives the measurement, so only a transient one ever runs its finalizer.
                if (_lifetime == target_lifetime::transient)
                    _finalizers[ring_slot()].push_back(
                        [this, key]
                        {
                            VkImageView doomed = VK_NULL_HANDLE;
                            _views.lock(
                                [&](view_map& v)
                                {
                                    if (auto const* e = v.get_ptr(key); e != nullptr)
                                    {
                                        doomed = *e;
                                        v.erase(key);
                                    }
                                });
                            if (doomed != VK_NULL_HANDLE)
                                vkDestroyImageView(_device, doomed, nullptr);
                        });
                return created;
            });
    }

    void end_frame() {}

    void drain()
    {
        for (auto& ring : _finalizers)
        {
            for (auto& f : ring)
                f();
            ring.clear();
        }
        _views.lock(
            [&](view_map& views)
            {
                for (auto const& [key, view] : views)
                    vkDestroyImageView(_device, view, nullptr);
                views.clear();
            });
    }

private:
    using view_map = cc::map<vulkan::vulkan_image_view_key, VkImageView>;

    cc::mutex<view_map> _views;
    cc::vector<cc::unique_function<void()>> _finalizers[k_epoch_ring];
};

/// The views one epoch must destroy once it retires, under the lock the epoch state takes at submit.
class epoch_retire_ring
{
public:
    void retire(VkDevice device, int slot)
    {
        _rings.lock(
            [&](rings& r)
            {
                for (auto const view : r.views[slot])
                    vkDestroyImageView(device, view, nullptr);
                r.views[slot].clear();
            });
    }

    /// A command list's collected views, handed over at submit.
    void submit(int slot, cc::vector<VkImageView>& list_views)
    {
        _rings.lock(
            [&](rings& r)
            {
                for (auto const view : list_views)
                    r.views[slot].push_back(view);
            });
        list_views.clear();
    }

    void drain(VkDevice device)
    {
        for (auto slot = 0; slot < k_epoch_ring; ++slot)
            retire(device, slot);
    }

private:
    struct rings
    {
        cc::vector<VkImageView> views[k_epoch_ring];
    };
    cc::mutex<rings> _rings;
};

/// acquire_attachment_view itself, collecting what it creates the way a recording command list does.
class texture_epoch_model : public frame_driver
{
public:
    texture_epoch_model(vulkan::vulkan_context& ctx, cc::span<target const> targets, target_lifetime lifetime)
      : frame_driver(ctx._device, targets, lifetime), _ctx(ctx)
    {
    }

    void begin_frame()
    {
        ++_frame;
        _retired.retire(_device, ring_slot());
        // A transient target is a new texture, and a new texture has no views yet.
        if (_lifetime == target_lifetime::transient)
            for (auto const& t : _targets)
                t.texture->_attachment_views.lock([](auto& views) { views.clear(); });
    }

    [[nodiscard]] VkImageView acquire(int index)
    {
        auto const& t = _targets[index];
        return vulkan::acquire_attachment_view(_ctx, *t.texture, sg::epoch(_frame), sg::texture_view_dimension::tex_2d,
                                               t.sg_format, t.range, _list_views);
    }

    void end_frame() { _retired.submit(ring_slot(), _list_views); }

    void drain()
    {
        _retired.drain(_device);
        for (auto const& t : _targets)
            t.texture->_attachment_views.lock([](auto& views) { views.clear(); });
    }

private:
    vulkan::vulkan_context& _ctx;
    cc::vector<VkImageView> _list_views; // the recording command list's own collection
    epoch_retire_ring _retired;
};

/// No cache at all: every acquire creates a view the epoch destroys.
class no_cache_model : public frame_driver
{
public:
    using frame_driver::frame_driver;

    void begin_frame()
    {
        ++_frame;
        _retired.retire(_device, ring_slot());
    }

    [[nodiscard]] VkImageView acquire(int index)
    {
        auto const view = create_view(_device, _targets[index]);
        _list_views.push_back(view);
        return view;
    }

    void end_frame() { _retired.submit(ring_slot(), _list_views); }

    void drain() { _retired.drain(_device); }

private:
    cc::vector<VkImageView> _list_views;
    epoch_retire_ring _retired;
};

template <class Model>
void measure(cc::string_view name, Model& model, int uses)
{
    nx::bench::run(name, {.min_time_secs = 0.2},
                   [&](nx::bench::iteration& it)
                   {
                       model.begin_frame();
                       for (auto use = 0; use < uses; ++use)
                           for (auto t = 0; t < k_targets; ++t)
                               nx::bench::sink(model.acquire(t));
                       model.end_frame();
                       it.items(k_targets * uses);
                   });
    model.drain();
}

struct validation_mode
{
    char const* name = "";
    bool layers = false;
    bool sync = false;
};

constexpr validation_mode k_validation_modes[] = {
    {.name = "no validation", .layers = false, .sync = false},
    {.name = "validation", .layers = true, .sync = false},
    {.name = "validation + sync", .layers = true, .sync = true},
};

void run_scenario(target_lifetime lifetime, int uses)
{
    for (auto const& mode : k_validation_modes)
    {
        auto created
            = sg::create_vulkan_context({.enable_validation_layers = mode.layers, .enable_sync_validation = mode.sync});
        if (created.has_error())
            SKIP("no vulkan device");
        auto const ctx = std::static_pointer_cast<vulkan::vulkan_context>(created.value());

        cc::vector<sg::texture_2d> textures;
        cc::vector<target> targets;
        for (auto i = 0; i < k_targets; ++i)
        {
            auto const is_depth = i == k_color_targets;
            auto const format = is_depth ? sg::pixel_format::depth32_float : sg::pixel_format::rgba8_unorm;
            textures.push_back(ctx->persistent.create_texture_2d(
                {.format = format,
                 .width = 256,
                 .height = 256,
                 .usage = is_depth ? sg::texture_usage::depth_stencil : sg::texture_usage::render_target}));
            auto const& vk_texture = static_cast<vulkan::vulkan_texture const&>(*textures.back().raw());
            targets.push_back(
                {.image = vk_texture._image,
                 .format = is_depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
                 .aspect = VkImageAspectFlags(is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                 .sg_format = format,
                 .texture = &vk_texture,
                 .range = is_depth ? textures.back().as_depth_stencil_view().range()
                                   : textures.back().as_render_target_view().range()});
        }

        auto const device = ctx->_device;
        auto const label = cc::format("{} - {}", ctx->metrics.adapter().name, mode.name);
        {
            auto model = context_map_model(device, targets, lifetime);
            measure(cc::format("{} - context map", label), model, uses);
        }
        {
            auto model = texture_epoch_model(*ctx, targets, lifetime);
            measure(cc::format("{} - texture epoch slot", label), model, uses);
        }
        {
            auto model = no_cache_model(device, targets, lifetime);
            measure(cc::format("{} - no cache", label), model, uses);
        }
    }
}
} // namespace

BENCHMARK("sg vulkan - attachment views - transient targets - 1 use per frame")
{
    run_scenario(target_lifetime::transient, 1);
}

BENCHMARK("sg vulkan - attachment views - transient targets - 4 uses per frame")
{
    run_scenario(target_lifetime::transient, 4);
}

BENCHMARK("sg vulkan - attachment views - persistent targets - 1 use per frame")
{
    run_scenario(target_lifetime::persistent, 1);
}

BENCHMARK("sg vulkan - attachment views - persistent targets - 4 uses per frame")
{
    run_scenario(target_lifetime::persistent, 4);
}
