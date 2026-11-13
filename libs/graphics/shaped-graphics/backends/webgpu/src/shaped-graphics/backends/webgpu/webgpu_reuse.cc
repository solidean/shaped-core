// webgpu reuse across epochs: transient textures and buffers on two sides, transient binding groups, and the view sweep.
//
// One rule for every kind: an object is handed back while it keeps being asked for, and released once an epoch has
// passed without a request (see webgpu_context.hh, "reuse across epochs").

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/container/map.hh>
#include <shaped-graphics/backends/webgpu/webgpu_binding_group_layout.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>

namespace sg::backend::webgpu
{
namespace
{
/// Whether a texture made for `a` serves a request for `b`: the whole description, since a transient texture's
/// contents are undefined anyway and nothing else about it may differ.
[[nodiscard]] bool same_description(sg::texture_description const& a, sg::texture_description const& b)
{
    return a.format == b.format && a.dimension == b.dimension && a.width == b.width && a.height == b.height
        && a.depth == b.depth && a.mip_levels == b.mip_levels && a.array_layers == b.array_layers
        && a.sample_count == b.sample_count && a.is_cube == b.is_cube && a.usage == b.usage
        && a.initial_layout == b.initial_layout;
}

[[nodiscard]] u64 hash_of(cc::span<u64 const> key)
{
    auto h = u64(0xcbf29ce484222325ull);
    for (auto const v : key)
    {
        h ^= v;
        h *= 0x100000001b3ull;
    }
    return h;
}

[[nodiscard]] bool same_key(cc::span<u64 const> a, cc::span<u64 const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

void webgpu_context::offer_texture(sg::texture_description const& desc, std::shared_ptr<webgpu_texture_storage> storage)
{
    // The two sides belong to the device thread; a handle dropped elsewhere offers its texture there.
    if (!is_on_device_thread())
    {
        impl::defer_to_main([this, desc, storage = cc::move(storage)]() mutable
                            { offer_texture(desc, cc::move(storage)); });
        return;
    }
    if (_is_shut_down || storage == nullptr || !storage->texture)
        return;
    _incoming_textures.push_back({.desc = desc, .storage = cc::move(storage)});
}

void webgpu_context::offer_buffer(isize size_in_bytes, WGPUBufferUsage usage, wgpu_buffer buffer, u64 serial)
{
    if (!is_on_device_thread())
    {
        impl::defer_to_main([this, size_in_bytes, usage, buffer = cc::move(buffer), serial]() mutable
                            { offer_buffer(size_in_bytes, usage, cc::move(buffer), serial); });
        return;
    }
    if (_is_shut_down || !buffer)
        return;
    _incoming_buffers.push_back(
        {.size_in_bytes = size_in_bytes, .usage = usage, .buffer = cc::move(buffer), .serial = serial});
}

std::shared_ptr<webgpu_texture_storage> webgpu_context::claim_texture(sg::texture_description const& desc)
{
    for (isize i = 0; i < _claimable_textures.size(); ++i)
        if (same_description(_claimable_textures[i].desc, desc))
        {
            auto storage = cc::move(_claimable_textures[i].storage);
            _claimable_textures.remove_at(i);
            return storage;
        }
    return nullptr;
}

webgpu_context::reusable_buffer webgpu_context::claim_buffer(isize size_in_bytes, WGPUBufferUsage usage)
{
    for (isize i = 0; i < _claimable_buffers.size(); ++i)
        if (_claimable_buffers[i].size_in_bytes == size_in_bytes && _claimable_buffers[i].usage == usage)
        {
            auto claimed = cc::move(_claimable_buffers[i]);
            _claimable_buffers.remove_at(i);
            return claimed;
        }
    return {};
}

void webgpu_context::sweep_views_of(std::shared_ptr<webgpu_texture_storage> const& storage)
{
    _view_sweep.push_back(storage);
}

wgpu_bind_group webgpu_context::claim_binding_group(webgpu_binding_group_layout_handle const& layout,
                                                    cc::span<u64 const> key)
{
    auto const hash = hash_of(key);
    for (auto const& g : _current_groups)
        if (g.hash == hash && g.layout == layout && same_key(g.key, key))
            return g.group;
    for (isize i = 0; i < _previous_groups.size(); ++i)
    {
        auto& g = _previous_groups[i];
        if (g.hash != hash || g.layout != layout || !same_key(g.key, key))
            continue;
        auto group = g.group;
        _current_groups.push_back(cc::move(g));
        _previous_groups.remove_at_unordered(i);
        return group;
    }
    return {};
}

void webgpu_context::keep_binding_group(webgpu_binding_group_layout_handle const& layout,
                                        cc::vector<u64> key,
                                        wgpu_bind_group const& group)
{
    auto const hash = hash_of(key);
    _current_groups.push_back({.layout = layout, .key = cc::move(key), .hash = hash, .group = group});
}

void webgpu_context::advance_reuse()
{
    // What nobody claimed in the epoch that just closed is released with it; it was last used in the one before.
    for (auto& t : _claimable_textures)
    {
        auto expiring = webgpu_expiring_resource{};
        expiring.texture = cc::move(t.storage->texture);
        schedule_deferred_deletion(cc::move(expiring));
    }
    for (auto& b : _claimable_buffers)
    {
        auto expiring = webgpu_expiring_resource{};
        expiring.buffer = cc::move(b.buffer);
        schedule_deferred_deletion(cc::move(expiring));
    }
    // In the order this epoch's requests were handed them, so the next epoch's same requests get the same objects.
    auto rank = cc::map<u64, isize>();
    for (auto i = isize(0); i < _handed_out.size(); ++i)
        rank[_handed_out[i]] = i;
    auto const rank_of = [&](u64 serial)
    {
        auto const* const r = rank.get_ptr(serial);
        return r != nullptr ? *r : _handed_out.size();
    };
    cc::sort_stable_by(_incoming_textures, [&](reusable_texture const& t) { return rank_of(t.storage->serial); });
    cc::sort_stable_by(_incoming_buffers, [&](reusable_buffer const& b) { return rank_of(b.serial); });
    _handed_out.clear();

    _claimable_textures = cc::move(_incoming_textures);
    _incoming_textures = {};
    _claimable_buffers = cc::move(_incoming_buffers);
    _incoming_buffers = {};

    // A group not claimed in the epoch that just closed is dropped now; the WebGPU object goes with its last reference.
    _previous_groups = cc::move(_current_groups);
    _current_groups = {};

    // A view over part of a texture -- a mip written once while a chain is generated -- goes once it is two epochs old.
    auto const now = current_epoch_number();
    auto kept = cc::vector<std::weak_ptr<webgpu_texture_storage>>();
    for (auto const& weak : _view_sweep)
    {
        auto const storage = weak.lock();
        if (storage == nullptr)
            continue;
        auto& views = storage->views;
        auto partial = false;
        for (isize i = views.size() - 1; i >= 0; --i)
        {
            if (views[i].whole)
                continue;
            if (views[i].last_used_epoch + 2 <= now)
                views.remove_at(i);
            else
                partial = true;
        }
        storage->swept = partial;
        if (partial)
            kept.push_back(weak);
    }
    _view_sweep = cc::move(kept);
}
} // namespace sg::backend::webgpu
