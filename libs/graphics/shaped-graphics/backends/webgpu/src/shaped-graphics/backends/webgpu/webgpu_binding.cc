// The bind path: binding group layouts, pipeline layouts with the reserved group 3, and binding groups.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/backends/webgpu/webgpu_format.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>

namespace sg::backend::webgpu
{
namespace
{
[[nodiscard]] WGPUStorageTextureAccess to_wgpu_storage_access(sg::access_mode access)
{
    switch (access)
    {
    case sg::access_mode::read:
        return WGPUStorageTextureAccess_ReadOnly;
    case sg::access_mode::write:
        return WGPUStorageTextureAccess_WriteOnly;
    case sg::access_mode::read_write:
        return WGPUStorageTextureAccess_ReadWrite;
    }
    return WGPUStorageTextureAccess_ReadWrite;
}

[[nodiscard]] bool is_multisampled(sg::texture_view_dimension dim)
{
    return dim == sg::texture_view_dimension::tex_2d_ms || dim == sg::texture_view_dimension::tex_2d_ms_array;
}

/// One layout entry for `b`, which must not be an array.
/// A sampler the layout binds itself is laid out as what it is, since a shader's reflection cannot tell a nearest sampler from a linear one.
[[nodiscard]] WGPUBindGroupLayoutEntry layout_entry_of(sg::binding const& b,
                                                       u32 binding_index,
                                                       cc::optional<WGPUSamplerBindingType> static_sampler_type)
{
    CC_ASSERTF(!(b.visibility.has(sg::shader_stage::vertex) && b.is_writable()),
               "'{}' is writable storage marked vertex-visible, and webgpu allows no writable storage in the vertex "
               "stage",
               b.name);

    auto entry = WGPUBindGroupLayoutEntry{};
    entry.binding = binding_index;
    // An unstated image stays out of the vertex stage even when only read, as it always has.
    entry.visibility = to_wgpu_visibility(b.visibility, b.is_writable() || b.type == sg::binding_type::image);
    entry.bindingArraySize = 0;

    switch (b.type)
    {
    case sg::binding_type::constants_buffer:
        entry.buffer.type = WGPUBufferBindingType_Uniform;
        entry.buffer.minBindingSize = u64(b.block_size.value_or(0));
        break;
    case sg::binding_type::buffer:
    case sg::binding_type::bytes:
        entry.buffer.type
            = b.access == sg::access_mode::read ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Storage;
        break;
    case sg::binding_type::texture:
    {
        auto const dim = b.texture_dimension.value_or(sg::texture_view_dimension::tex_2d);
        auto const multisampled = is_multisampled(dim);
        entry.texture.sampleType
            = b.sample_type.has_value() ? to_wgpu_sample_type(b.sample_type.value()) : WGPUTextureSampleType_Float;
        // WebGPU never filters a multisampled texture and refuses a layout that says it might.
        if (multisampled && entry.texture.sampleType == WGPUTextureSampleType_Float)
            entry.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        entry.texture.viewDimension = to_wgpu_view_dimension(dim);
        entry.texture.multisampled = multisampled ? WGPU_TRUE : WGPU_FALSE;
        break;
    }
    case sg::binding_type::image:
        entry.storageTexture.access = to_wgpu_storage_access(b.access);
        entry.storageTexture.format = to_wgpu_format(b.image_format.value_or(sg::pixel_format::undefined));
        entry.storageTexture.viewDimension
            = to_wgpu_view_dimension(b.texture_dimension.value_or(sg::texture_view_dimension::tex_2d));
        break;
    case sg::binding_type::sampler:
        entry.sampler.type = static_sampler_type.has_value() ? static_sampler_type.value()
                           : b.sampler_type.has_value()      ? to_wgpu_sampler_binding_type(b.sampler_type.value())
                                                             : WGPUSamplerBindingType_Filtering;
        break;
    case sg::binding_type::acceleration_structure:
        CC_UNREACHABLE("an acceleration structure takes no webgpu layout entry");
    }
    return entry;
}

[[nodiscard]] wgpu_bind_group_layout create_layout(WGPUDevice device,
                                                   cc::span<WGPUBindGroupLayoutEntry const> entries,
                                                   char const* label)
{
    auto const desc = WGPUBindGroupLayoutDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu(label),
        .entryCount = size_t(entries.size()),
        .entries = entries.empty() ? nullptr : entries.data(),
    };
    return wgpu_bind_group_layout(wgpuDeviceCreateBindGroupLayout(device, &desc));
}
} // namespace

// -- binding group layout --

webgpu_binding_group_layout::webgpu_binding_group_layout(webgpu_context& ctx,
                                                         cc::hash128 hash,
                                                         cc::vector<sg::binding> bindings,
                                                         cc::vector<sg::named_sampler> static_samplers,
                                                         cc::vector<WGPUBindGroupLayoutEntry> entries,
                                                         cc::vector<cc::optional<sg::sampler>> slot_sampler_descs,
                                                         cc::vector<isize> acceleration_slots)
  : sg::binding_group_layout(hash, cc::move(bindings), cc::move(static_samplers)),
    _ctx(ctx),
    _entries(cc::move(entries)),
    _slot_sampler_descs(cc::move(slot_sampler_descs)),
    _acceleration_slots(cc::move(acceleration_slots))
{
}

cc::result<webgpu_binding_group_layout_handle> webgpu_binding_group_layout::create(
    webgpu_context& ctx,
    cc::span<sg::binding const> bindings,
    cc::span<sg::named_sampler const> static_samplers)
{
    auto const hash = sg::impl::binding_group_layout_hash(bindings, static_samplers);

    for (isize i = 0; i < bindings.size(); ++i)
    {
        auto const& b = bindings[i];
        if (b.count != 1)
            return cc::error(cc::format("binding_group_layout: '{}' is an array binding (count {}), and webgpu has no "
                                        "binding arrays (ctx.supports(sg::feature::binding_arrays) is false)",
                                        b.name, b.count));
        if (b.type == sg::binding_type::image
            && b.image_format.value_or(sg::pixel_format::undefined) == sg::pixel_format::undefined)
            return cc::error(cc::format("binding_group_layout: image '{}' declares no image_format, which "
                                        "a "
                                        "webgpu layout needs before any view exists",
                                        b.name));
        if (b.type == sg::binding_type::image && b.access == sg::access_mode::read_write
            && !ctx.supports(sg::feature::readwrite_image_formats))
        {
            auto const format = b.image_format.value_or(sg::pixel_format::undefined);
            if (format != sg::pixel_format::r32_float && format != sg::pixel_format::r32_uint
                && format != sg::pixel_format::r32_sint)
                return cc::error(cc::format("binding_group_layout: image '{}' is read_write in a format "
                                            "other "
                                            "than r32float / r32uint / r32sint, which needs "
                                            "sg::feature::readwrite_image_formats (webgpu's texture-formats-tier2), "
                                            "and this device lacks it; declare it write or read instead",
                                            b.name));
        }
        if (b.type == sg::binding_type::texture && b.texture_dimension == sg::texture_view_dimension::tex_2d_ms_array)
            return cc::error(cc::format("binding_group_layout: '{}' is a multisampled array texture, which webgpu does "
                                        "not have (ctx.supports(sg::feature::multisampled_array_textures) is false)",
                                        b.name));
        for (isize j = i + 1; j < bindings.size(); ++j)
            if (bindings[j].index == b.index)
                return cc::error(cc::format("binding_group_layout: '{}' and '{}' are both at @binding({})", b.name,
                                            bindings[j].name, b.index));
    }

    for (auto const& s : static_samplers)
    {
        auto matched = false;
        for (auto const& b : bindings)
            matched = matched || (sg::is_sampler(b.type) && b.name == s.name);
        if (!matched)
            return cc::error(cc::format("binding_group_layout: static sampler '{}' names no sampler binding", s.name));
    }

    auto slot_sampler_descs = cc::vector<cc::optional<sg::sampler>>::create_filled(bindings.size(), cc::nullopt);
    auto entries = cc::vector<WGPUBindGroupLayoutEntry>();
    auto acceleration_slots = cc::vector<isize>();
    entries.reserve(bindings.size());
    for (isize i = 0; i < bindings.size(); ++i)
    {
        auto const& b = bindings[i];
        if (b.type == sg::binding_type::acceleration_structure)
        {
            auto at = isize(0);
            while (at < acceleration_slots.size() && bindings[acceleration_slots[at]].index < b.index)
                ++at;
            acceleration_slots.insert_at(at, i);
            continue;
        }
        auto sampler_type = cc::optional<WGPUSamplerBindingType>();
        if (sg::is_sampler(b.type))
            for (auto const& s : static_samplers)
                if (s.name == b.name)
                {
                    slot_sampler_descs[i] = s.sampler;
                    sampler_type = default_sampler_binding_type(s.sampler);
                    break;
                }
        entries.push_back(layout_entry_of(b, b.index, sampler_type));
    }

    return webgpu_binding_group_layout_handle(std::make_shared<webgpu_binding_group_layout>(
        ctx, hash, cc::vector<sg::binding>::create_copy_of(bindings),
        cc::vector<sg::named_sampler>::create_copy_of(static_samplers), cc::move(entries), cc::move(slot_sampler_descs),
        cc::move(acceleration_slots)));
}

void webgpu_binding_group_layout::materialize() const
{
    if (_layout)
        return;

    _slot_samplers = cc::vector<WGPUSampler>::create_filled(_slot_sampler_descs.size(), nullptr);
    for (isize i = 0; i < _slot_sampler_descs.size(); ++i)
        if (_slot_sampler_descs[i].has_value())
            _slot_samplers[i] = _ctx._samplers.acquire(_slot_sampler_descs[i].value());

    _layout = create_layout(_ctx.device(), _entries, "sg binding group layout");
    CC_ASSERT(bool(_layout), "wgpuDeviceCreateBindGroupLayout returned no layout for a layout sg validated");
}

WGPUBindGroupLayout webgpu_binding_group_layout::raw() const
{
    materialize();
    return _layout.get();
}

cc::span<WGPUSampler const> webgpu_binding_group_layout::slot_samplers() const
{
    materialize();
    return _slot_samplers;
}

// -- pipeline layout --

webgpu_pipeline_layout::webgpu_pipeline_layout(webgpu_context& ctx,
                                               cc::hash128 hash,
                                               sg::pipeline_layout_description const& desc)
  : sg::pipeline_layout(hash, desc.groups, desc.inline_constants), _ctx(ctx)
{
}

cc::result<webgpu_pipeline_layout_handle> webgpu_pipeline_layout::create(webgpu_context& ctx,
                                                                         sg::pipeline_layout_description const& desc)
{
    if (int(desc.groups.size()) > sg::max_binding_groups)
        return cc::error("pipeline_layout: more group slots than max_binding_groups");

    auto layout = std::make_shared<webgpu_pipeline_layout>(ctx, sg::impl::pipeline_layout_hash(desc), desc);
    auto acceleration_count = isize(0);
    for (auto const& group : desc.groups)
    {
        CC_ASSERT(group != nullptr, "a pipeline layout's group is null");
        auto webgpu_group = std::dynamic_pointer_cast<webgpu_binding_group_layout const>(group);
        CC_ASSERT(webgpu_group != nullptr, "binding group layout is not a webgpu one");
        layout->_acceleration_bases.push_back(acceleration_count);
        acceleration_count += webgpu_group->acceleration_slots().size();
        layout->_groups.push_back(cc::move(webgpu_group));
    }
    if (acceleration_count > max_acceleration_members)
        return cc::error(cc::format("pipeline_layout: {} acceleration structures, and the webgpu ray-query polyfill "
                                    "binds "
                                    "at most {}",
                                    acceleration_count, max_acceleration_members));
    layout->_has_acceleration = acceleration_count > 0;

    // Group 3: the inline constants at binding 0, the register-bound samplers at their index + 1.
    auto& reserved = layout->_reserved_entries;
    if (desc.inline_constants.has_value())
    {
        auto const& b = desc.inline_constants.value();
        CC_ASSERT(b.type == sg::binding_type::constants_buffer, "inline constants must be a constants_buffer binding");
        CC_ASSERT(b.block_size.has_value() && b.block_size.value() > 0 && b.block_size.value() % 4 == 0,
                  "inline constants need a block_size that is a positive multiple of 4");
        layout->_inline_constants_bytes = b.block_size.value();

        auto entry = WGPUBindGroupLayoutEntry{};
        entry.binding = 0;
        entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        entry.buffer.type = WGPUBufferBindingType_Uniform;
        entry.buffer.hasDynamicOffset = WGPU_TRUE;
        entry.buffer.minBindingSize = u64(layout->_inline_constants_bytes);
        reserved.push_back(entry);
    }
    for (isize i = 0; i < desc.static_samplers.size(); ++i)
    {
        // sg refused a malformed or colliding one before the backend saw it.
        auto const& s = desc.static_samplers[i];
        auto const binding_index = s.binding.index + 1;
        auto entry = layout_entry_of(s.binding, binding_index, default_sampler_binding_type(s.sampler));
        entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        reserved.push_back(entry);
        layout->_reserved_samplers.push_back({.binding = binding_index, .desc = s.sampler});
    }

    // The ray-query polyfill's pool and roots, per libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md.
    if (layout->_has_acceleration)
    {
        auto pool = WGPUBindGroupLayoutEntry{};
        pool.binding = acceleration_pool_binding;
        pool.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        pool.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        reserved.push_back(pool);

        auto roots = WGPUBindGroupLayoutEntry{};
        roots.binding = acceleration_roots_binding;
        roots.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        roots.buffer.type = WGPUBufferBindingType_Uniform;
        roots.buffer.hasDynamicOffset = WGPU_TRUE;
        roots.buffer.minBindingSize = u64(acceleration_roots_bytes);
        reserved.push_back(roots);

        layout->_roots_offset_in_block = layout->_inline_constants_bytes == 0
                                           ? 0
                                           : align_up(layout->_inline_constants_bytes, ctx.uniform_offset_alignment());
        layout->_reserved_block_bytes = layout->_roots_offset_in_block + acceleration_roots_bytes;
    }
    else
        layout->_reserved_block_bytes = layout->_inline_constants_bytes;

    layout->_slot_count = reserved.empty() ? int(layout->_groups.size()) : sg::reserved_binding_group + 1;
    return webgpu_pipeline_layout_handle(cc::move(layout));
}

void webgpu_pipeline_layout::materialize() const
{
    if (_layout)
        return;

    auto set_layouts = cc::vector<WGPUBindGroupLayout>();
    for (auto const& g : _groups)
        set_layouts.push_back(g->raw());

    if (!_reserved_entries.empty())
    {
        for (auto& s : _reserved_samplers)
            s.sampler = _ctx._samplers.acquire(s.desc);
        _reserved_layout = create_layout(_ctx.device(), _reserved_entries, "sg reserved group");
        _empty_layout = create_layout(_ctx.device(), {}, "sg empty group");
        auto const empty_desc = WGPUBindGroupDescriptor{
            .nextInChain = nullptr,
            .label = to_wgpu("sg empty group"),
            .layout = _empty_layout.get(),
            .entryCount = 0,
            .entries = nullptr,
        };
        _empty_group = wgpu_bind_group(wgpuDeviceCreateBindGroup(_ctx.device(), &empty_desc));
        while (set_layouts.size() < sg::reserved_binding_group)
            set_layouts.push_back(_empty_layout.get());
        set_layouts.push_back(_reserved_layout.get());
    }
    CC_ASSERT(int(set_layouts.size()) == _slot_count, "the slot count create computed is the one materialized");

    auto const pipeline_desc = WGPUPipelineLayoutDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg pipeline layout"),
        .bindGroupLayoutCount = size_t(set_layouts.size()),
        .bindGroupLayouts = set_layouts.empty() ? nullptr : set_layouts.data(),
        .immediateSize = 0,
    };
    _layout = wgpu_pipeline_layout(wgpuDeviceCreatePipelineLayout(_ctx.device(), &pipeline_desc));
    CC_ASSERT(bool(_layout), "wgpuDeviceCreatePipelineLayout returned no layout for a layout sg validated");
}

WGPUPipelineLayout webgpu_pipeline_layout::raw() const
{
    materialize();
    return _layout.get();
}

WGPUBindGroup webgpu_pipeline_layout::empty_group() const
{
    materialize();
    return _empty_group.get();
}

WGPUBindGroup webgpu_pipeline_layout::reserved_group_for(webgpu_constant_page const* page) const
{
    CC_ASSERT(has_reserved_group(), "this pipeline layout has no reserved group");
    materialize();

    if (_has_acceleration)
    {
        // Before the compare, since the first use creates the pool and so moves its generation.
        (void)_ctx._acceleration.buffer();
        if (_reserved_groups_pool_generation != _ctx._acceleration.generation())
        {
            _reserved_groups.clear();
            _reserved_group_without_page = {};
            _reserved_groups_pool_generation = _ctx._acceleration.generation();
        }
    }

    auto const build = [&](WGPUBuffer page_buffer)
    {
        auto entries = cc::vector<WGPUBindGroupEntry>();
        if (_inline_constants_bytes > 0)
        {
            auto entry = WGPUBindGroupEntry{};
            entry.binding = 0;
            entry.buffer = page_buffer;
            entry.offset = 0;
            entry.size = u64(_inline_constants_bytes);
            entries.push_back(entry);
        }
        for (auto const& s : _reserved_samplers)
        {
            auto entry = WGPUBindGroupEntry{};
            entry.binding = s.binding;
            entry.sampler = s.sampler;
            entries.push_back(entry);
        }
        if (_has_acceleration)
        {
            auto pool = WGPUBindGroupEntry{};
            pool.binding = acceleration_pool_binding;
            pool.buffer = _ctx._acceleration.buffer().get();
            pool.offset = 0;
            pool.size = WGPU_WHOLE_SIZE;
            entries.push_back(pool);

            auto roots = WGPUBindGroupEntry{};
            roots.binding = acceleration_roots_binding;
            roots.buffer = page_buffer;
            roots.offset = 0;
            roots.size = u64(acceleration_roots_bytes);
            entries.push_back(roots);
        }
        auto const desc = WGPUBindGroupDescriptor{
            .nextInChain = nullptr,
            .label = to_wgpu("sg reserved group"),
            .layout = _reserved_layout.get(),
            .entryCount = size_t(entries.size()),
            .entries = entries.empty() ? nullptr : entries.data(),
        };
        return wgpu_bind_group(wgpuDeviceCreateBindGroup(_ctx.device(), &desc));
    };

    if (_reserved_block_bytes == 0)
    {
        if (!_reserved_group_without_page)
            _reserved_group_without_page = build(nullptr);
        return _reserved_group_without_page.get();
    }

    CC_ASSERT(page != nullptr, "the reserved block needs a page to bind");
    while (_reserved_groups.size() <= page->index)
        _reserved_groups.push_back({});
    auto& group = _reserved_groups[page->index];
    if (!group)
        group = build(page->buffer.get());
    return group.get();
}

isize webgpu_pipeline_layout::reserved_dynamic_offsets(u32 block_offset, cc::span<u32> out) const
{
    auto count = isize(0);
    if (_inline_constants_bytes > 0)
        out[count++] = block_offset;
    if (_has_acceleration)
        out[count++] = block_offset + u32(_roots_offset_in_block);
    return count;
}

// -- binding group --

namespace
{
struct resolved_view
{
    isize slot = 0;
    cc::string_view name;
    sg::bound_view const* view = nullptr;
};

[[nodiscard]] cc::result<webgpu_binding_group_handle> create_resolved(webgpu_context& ctx,
                                                                      webgpu_binding_group_layout_handle const& layout,
                                                                      cc::span<resolved_view const> views,
                                                                      cc::span<sg::named_sampler const> samplers,
                                                                      sg::lifetime_scope scope)
{
    ctx.assert_on_device_thread();
    auto group = std::make_shared<webgpu_binding_group>();
    group->layout = layout;
    group->transient = scope == sg::lifetime_scope::transient;
    group->creation_epoch = ctx.current_epoch();

    auto const bindings = layout->bindings();
    auto entries = cc::vector<WGPUBindGroupEntry>();
    auto texture_views = cc::vector<wgpu_texture_view>();
    auto filled = cc::vector<char>::create_filled(bindings.size(), char(0));
    group->acceleration_roots = cc::vector<u32>::create_filled(layout->acceleration_slots().size(), u32(0));

    // A static sampler binds the layout's own sampler; the caller never supplies one.
    for (isize i = 0; i < bindings.size(); ++i)
        if (layout->slot_samplers()[i] != nullptr)
        {
            auto entry = WGPUBindGroupEntry{};
            entry.binding = bindings[i].index;
            entry.sampler = layout->slot_samplers()[i];
            entries.push_back(entry);
            filled[i] = char(1);
        }

    for (auto const& rv : views)
    {
        auto const& b = bindings[rv.slot];
        auto const elements = rv.view->span();
        if (elements.size() != 1)
            return cc::error(cc::format("binding_group: '{}' takes one view, {} provided (webgpu has no binding "
                                        "arrays)",
                                        rv.name, elements.size()));
        CC_ASSERT(filled[rv.slot] == char(0), "binding_group: a binding was provided more than once");
        filled[rv.slot] = char(1);

        auto const& view = elements[0];
        if (sg::is_vacant(view))
            return cc::error(cc::format("binding_group: '{}' is vacant, which only an array element may be", rv.name));
        if (!sg::accepts(b, view))
            return cc::error(
                cc::format("binding_group: the view bound to '{}' does not match its declared kind", rv.name));

        // An acceleration structure is a root in the pool, which the list writes into group 3; it has no entry here.
        if (auto const* av = sg::try_as_tlas_view(view))
        {
            auto root = u32(0);
            if (av->tlas != nullptr)
            {
                auto const* tlas = dynamic_cast<webgpu_tlas const*>(av->tlas.get());
                CC_ASSERT(tlas != nullptr, "bound tlas is not a webgpu tlas");
                CC_ASSERT(!tlas->is_expired(), "binding_group names an expired tlas");
                root = tlas->unit();
                group->referenced_tlases.push_back(av->tlas);
            }
            auto const slots = layout->acceleration_slots();
            for (isize k = 0; k < slots.size(); ++k)
                if (slots[k] == rv.slot)
                    group->acceleration_roots[k] = root;
            continue;
        }

        auto entry = WGPUBindGroupEntry{};
        entry.binding = b.index;
        if (auto const* bv = sg::try_as_buffer_view(view))
        {
            if (bv->buffer == nullptr)
                return cc::error(cc::format("binding_group: '{}' — a buffer view must bind a buffer", rv.name));
            auto const buffer = std::dynamic_pointer_cast<webgpu_buffer const>(bv->buffer);
            CC_ASSERT(buffer != nullptr, "bound buffer is not a webgpu buffer");
            CC_ASSERT(!buffer->is_expired(), "binding_group names an expired buffer");
            CC_ASSERTF(b.type != sg::binding_type::constants_buffer
                           || bv->offset_in_bytes % ctx.uniform_offset_alignment() == 0,
                       "binding_group: uniform buffer '{}' is bound at offset {}, which is not a multiple of the "
                       "device's minUniformBufferOffsetAlignment ({})",
                       rv.name, bv->offset_in_bytes, ctx.uniform_offset_alignment());
            auto const size
                = bv->shape == sg::view_shape::structured ? bv->element_count * bv->stride_in_bytes : bv->size_in_bytes;
            entry.buffer = buffer->raw();
            entry.offset = u64(bv->offset_in_bytes);
            entry.size = u64(size);
            group->referenced_buffers.push_back(bv->buffer);
            group->bound.push_back({.resource = bv->buffer.get(), .binding = isize(rv.slot), .bound_as = bv->bound_as});
        }
        else if (auto const* tv = sg::try_as_texture_view(view))
        {
            if (tv->texture == nullptr)
                return cc::error(cc::format("binding_group: '{}' — a texture view must bind a texture", rv.name));
            auto const texture = std::dynamic_pointer_cast<webgpu_texture const>(tv->texture);
            CC_ASSERT(texture != nullptr, "bound texture is not a webgpu texture");
            CC_ASSERT(!texture->is_expired(), "binding_group names an expired texture");
            texture_views.push_back(texture->create_view(tv->view_dimension, tv->format, tv->range));
            entry.textureView = texture_views.back().get();
            group->referenced_textures.push_back(tv->texture);
            group->bound.push_back({.resource = tv->texture.get(), .binding = isize(rv.slot), .bound_as = tv->bound_as});
        }
        else
            CC_UNREACHABLE("a view is a buffer, a texture or an acceleration structure");
        entries.push_back(entry);
    }

    for (auto const& ns : samplers)
    {
        auto slot = isize(-1);
        for (isize i = 0; i < bindings.size(); ++i)
            if (sg::is_sampler(bindings[i].type) && bindings[i].name == ns.name)
            {
                slot = i;
                break;
            }
        if (slot < 0)
            return cc::error(cc::format("binding_group: no sampler binding named '{}' in the layout", ns.name));
        if (layout->slot_samplers()[slot] != nullptr)
            return cc::error(cc::format("binding_group: sampler '{}' is static — it is fixed by the layout and must "
                                        "not be supplied per group",
                                        ns.name));
        CC_ASSERT(filled[slot] == char(0), "binding_group: a sampler was provided more than once");
        filled[slot] = char(1);

        auto entry = WGPUBindGroupEntry{};
        entry.binding = bindings[slot].index;
        entry.sampler = ctx._samplers.acquire(ns.sampler);
        entries.push_back(entry);
    }

    for (isize i = 0; i < bindings.size(); ++i)
        if (filled[i] == char(0))
            return cc::error(cc::format("binding_group: {} '{}' was not provided",
                                        sg::is_sampler(bindings[i].type) ? "sampler" : "binding", bindings[i].name));

    auto const desc = WGPUBindGroupDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg binding group"),
        .layout = layout->raw(),
        .entryCount = size_t(entries.size()),
        .entries = entries.empty() ? nullptr : entries.data(),
    };
    group->_group = wgpu_bind_group(wgpuDeviceCreateBindGroup(ctx.device(), &desc));
    if (!group->_group)
        return cc::error("wgpuDeviceCreateBindGroup returned no group");
    return webgpu_binding_group_handle(cc::move(group));
}
} // namespace

cc::result<webgpu_binding_group_handle> webgpu_binding_group::create(webgpu_context& ctx,
                                                                     webgpu_binding_group_layout_handle const& layout,
                                                                     cc::span<sg::named_view const> views,
                                                                     cc::span<sg::named_sampler const> samplers,
                                                                     sg::lifetime_scope scope)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");
    auto const bindings = layout->bindings();
    auto resolved = cc::vector<resolved_view>();
    for (auto const& nv : views)
    {
        auto slot = isize(-1);
        for (isize i = 0; i < bindings.size(); ++i)
            if (!sg::is_sampler(bindings[i].type) && bindings[i].name == nv.name)
            {
                slot = i;
                break;
            }
        if (slot < 0)
            return cc::error(cc::format("binding_group: no view binding named '{}' in the layout", nv.name));
        resolved.push_back({.slot = slot, .name = nv.name, .view = &nv.view});
    }
    return create_resolved(ctx, layout, resolved, samplers, scope);
}

cc::result<webgpu_binding_group_handle> webgpu_binding_group::create(webgpu_context& ctx,
                                                                     webgpu_binding_group_layout_handle const& layout,
                                                                     cc::span<sg::slotted_view const> views,
                                                                     cc::span<sg::named_sampler const> samplers,
                                                                     sg::lifetime_scope scope)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");
    auto const bindings = layout->bindings();
    auto resolved = cc::vector<resolved_view>();
    for (auto const& sv : views)
    {
        auto const slot = isize(u32(sv.slot));
        if (sv.slot == sg::binding_slot::invalid || slot >= bindings.size())
            return cc::error(cc::format("binding_group: slot {} is not a position in this layout's bindings()", slot));
        if (sg::is_sampler(bindings[slot].type))
            return cc::error(cc::format("binding_group: '{}' is a sampler, not a view", bindings[slot].name));
        resolved.push_back({.slot = slot, .name = bindings[slot].name, .view = &sv.view});
    }
    return create_resolved(ctx, layout, resolved, samplers, scope);
}

webgpu_binding_group_layout_handle webgpu_context::as_webgpu_layout(sg::binding_group_layout_handle const& layout)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");
    auto webgpu_layout = std::dynamic_pointer_cast<webgpu_binding_group_layout const>(layout);
    CC_ASSERT(webgpu_layout != nullptr, "binding_group_layout is not a webgpu one");
    return webgpu_layout;
}
} // namespace sg::backend::webgpu
