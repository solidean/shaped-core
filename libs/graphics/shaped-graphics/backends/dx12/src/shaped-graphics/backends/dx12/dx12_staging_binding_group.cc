// dx12_staging_binding_group: the mutable descriptor image behind sg::staging_binding_group.
// See dx12_staging_binding_group.hh and libs/graphics/shaped-graphics/docs/concepts/bindings.md.

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_acceleration_structure.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_sampler.hh>
#include <shaped-graphics/backends/dx12/dx12_staging_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_view_desc.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/resource/views.hh>

namespace sg::backend::dx12
{
namespace
{
// A ComPtr cannot ride inside a cc::result (its ComPtrRef makes the conversion to void* ambiguous), so the
// heap comes back through `out`.
[[nodiscard]] cc::result<cc::unit> create_staging_heap(ID3D12Device* device,
                                                       D3D12_DESCRIPTOR_HEAP_TYPE heap_type,
                                                       int count,
                                                       ComPtr<ID3D12DescriptorHeap>& out)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = heap_type;
    desc.NumDescriptors = UINT(count);
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // the copy source of CopyDescriptorsSimple must not be shader-visible

    if (HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&out)); FAILED(hr))
        return dx12_error(hr, "CreateDescriptorHeap (staging_binding_group, non-shader-visible) failed");
    return cc::unit{};
}

// Where each of the layout's bindings keeps its first descriptor, in declaration order — the base's slot map.
// A static sampler is in neither table and gets -1.
[[nodiscard]] cc::vector<int> descriptor_offsets_of(dx12_binding_group_layout const& layout)
{
    auto offsets = cc::vector<int>();
    for (auto const& b : layout.bindings())
    {
        auto const& slots = sg::is_sampler(b.type) ? layout.sampler_slots : layout.view_slots;
        int offset = -1;
        for (auto const& s : slots)
            if (s.binding.name == b.name)
            {
                offset = s.table_offset;
                break;
            }
        offsets.push_back(offset);
    }
    return offsets;
}
} // namespace

dx12_staging_binding_group::dx12_staging_binding_group(dx12_context& ctx,
                                                       dx12_binding_group_layout_handle layout,
                                                       cc::vector<int> descriptor_offsets)
  : sg::staging_binding_group(layout, cc::move(descriptor_offsets)), // base first, so the move below is safe
    _ctx(ctx),
    _dx_layout(cc::move(layout))
{
}

dx12_staging_binding_group::~dx12_staging_binding_group() = default;

cc::result<dx12_staging_binding_group_handle> dx12_staging_binding_group::create(dx12_context& ctx,
                                                                                 dx12_binding_group_layout_handle layout)
{
    CC_ASSERT(layout != nullptr, "staging_binding_group requires a binding_group_layout");

    // The two array kinds a binding_group cannot express either — rejected here rather than at the first set,
    // so a layout that can never be staged fails where it is handed over.
    for (auto const& s : layout->view_slots)
        if (s.binding.is_array() && s.binding.type == sg::binding_type::acceleration_structure)
            return cc::error("staging_binding_group: acceleration-structure arrays are not supported yet");
    for (auto const& s : layout->sampler_slots)
        if (s.binding.is_array())
            return cc::error("staging_binding_group: dynamic sampler arrays are not supported yet");

    auto offsets = descriptor_offsets_of(*layout);
    auto group = std::make_shared<dx12_staging_binding_group>(ctx, cc::move(layout), cc::move(offsets));
    auto r = group->initialize();
    CC_RETURN_IF_ERROR(r);
    return dx12_staging_binding_group_handle(cc::move(group));
}

cc::result<cc::unit> dx12_staging_binding_group::initialize()
{
    auto* const device = _ctx._device.Get();

    int const view_count = _dx_layout->descriptor_count;
    int const sampler_count = _dx_layout->sampler_descriptor_count;

    if (view_count > 0)
    {
        auto r = create_staging_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, view_count, _view_heap);
        CC_RETURN_IF_ERROR(r);
        _view_start = _view_heap->GetCPUDescriptorHandleForHeapStart();
        _view_increment = int(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
        for (int i = 0; i < view_count; ++i)
            _resources.push_back({});
    }
    if (sampler_count > 0)
    {
        auto r = create_staging_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, sampler_count, _sampler_heap);
        CC_RETURN_IF_ERROR(r);
        _sampler_start = _sampler_heap->GetCPUDescriptorHandleForHeapStart();
        _sampler_increment = int(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER));
    }

    // Every descriptor starts at its binding's empty value, so a snapshot is copyable and bindable from the
    // first one — an unwritten descriptor in a bound table is a D3D12 validation error.
    for (auto const& s : _dx_layout->view_slots)
        for (int e = 0; e < int(s.binding.count); ++e)
            create_null_view(device, s.binding, view_cpu_at(s.table_offset + e));
    for (auto const& s : _dx_layout->sampler_slots)
        create_null_view(device, s.binding, sampler_cpu_at(s.table_offset));

    return cc::unit{};
}

void dx12_staging_binding_group::write_view_descriptors(int first_descriptor,
                                                        [[maybe_unused]] sg::binding const& b,
                                                        cc::span<sg::raw_view const> views)
{
    // One dispatch for the whole run: the device and the heap stride are hoisted out, and only the per-descriptor
    // Create*View remains, which D3D12 has no batched form of.
    auto* const device = _ctx._device.Get();

    for (isize i = 0; i < views.size(); ++i)
    {
        auto const& view = views[i];
        auto const dst = view_cpu_at(first_descriptor + int(i));

        // Overwriting releases whatever the descriptor referenced before — that release is the point of staging.
        auto& res = _resources[first_descriptor + i];
        res = {};

        // Visited by arm, so a new raw_view arm has to say what descriptor it writes rather than fall into a default.
        view.visit(
            [&](sg::raw_buffer_view const& bv)
            {
                create_buffer_view(device, bv, dst);
                auto dx = std::dynamic_pointer_cast<dx12_buffer const>(bv.buffer);
                CC_ASSERT(dx != nullptr, "bound buffer is not a dx12 buffer");
                res.buffer = cc::move(dx);
                res.access = bv.access;
            },
            [&](sg::raw_texture_view const& tv)
            {
                create_texture_view(device, tv, dst);
                auto dx = std::dynamic_pointer_cast<dx12_texture const>(tv.texture);
                CC_ASSERT(dx != nullptr, "bound texture is not a dx12 texture");
                res.texture = cc::move(dx);
                res.range = tv.range;
                res.access = tv.access;
            },
            [&](sg::raw_tlas_view const& av)
            {
                // A null handle is the null acceleration structure, not a mistake: a valid descriptor every ray
                // misses, with no storage to keep alive and nothing to declare a hazard on.
                auto dx_tlas = std::shared_ptr<dx12_tlas const>();
                if (av.tlas != nullptr)
                {
                    dx_tlas = std::dynamic_pointer_cast<dx12_tlas const>(av.tlas);
                    CC_ASSERT(dx_tlas != nullptr, "bound acceleration structure is not a dx12 tlas");
                }
                create_accel_view(device, dx_tlas.get(), dst);
                if (dx_tlas != nullptr)
                {
                    res.buffer = dx_tlas->_dx12_storage;
                    res.access = sg::view_class::acceleration_structure;
                }
            },
            [](sg::vacant_view const&)
            { CC_ASSERT(false, "a written run carries no vacant view — clearing goes through clear_view_descriptors"); });
    }
    // `b` goes unused here: only the empty descriptor is synthesized from the binding, and a written run has a view for every descriptor.
    // It stays in the signature for a backend whose write does need it.
}

void dx12_staging_binding_group::clear_view_descriptors(int first_descriptor, sg::binding const& b, int count)
{
    // The empty descriptor is synthesized from the binding alone, so the whole run writes the same thing.
    auto* const device = _ctx._device.Get();
    for (int i = 0; i < count; ++i)
    {
        create_null_view(device, b, view_cpu_at(first_descriptor + i));
        _resources[first_descriptor + i] = {};
    }
}

void dx12_staging_binding_group::write_sampler_descriptor(int descriptor_index, sg::sampler const& smp)
{
    D3D12_SAMPLER_DESC const desc = to_d3d12_sampler_desc(smp);
    _ctx._device->CreateSampler(&desc, sampler_cpu_at(descriptor_index));
}

cc::result<sg::binding_group_handle> dx12_staging_binding_group::mint()
{
    auto group = std::make_shared<dx12_binding_group>();
    // Set before any allocation: a partial mint is then freed by the group's own destructor.
    group->_ctx = &_ctx;
    group->layout = _dx_layout;
    group->transient = false;
    group->creation_epoch = _ctx.current_epoch();

    // One CopyDescriptorsSimple per heap replaces a Create*View per descriptor — the reason a large table is
    // affordable to re-snapshot at all.
    auto const fill = [&](dx12_descriptor_heap& heap, ComPtr<ID3D12DescriptorHeap> const& staging, int count,
                          D3D12_DESCRIPTOR_HEAP_TYPE heap_type, D3D12_CPU_DESCRIPTOR_HANDLE staging_start,
                          dx12_descriptor_alloc& table, D3D12_GPU_DESCRIPTOR_HANDLE& table_start) -> cc::result<cc::unit>
    {
        if (count == 0)
            return cc::unit{};
        CC_ASSERT(staging != nullptr, "a non-empty table needs its staging heap");

        dx12_descriptor_alloc a = heap.allocate_persistent(count);
        if (a.is_empty())
            return cc::error("staging_binding_group: persistent descriptor heap exhausted (no free span fits the "
                             "table)");
        table = cc::move(a);
        table_start = heap.gpu_at(table.offset);
        _ctx._device->CopyDescriptorsSimple(UINT(count), heap.cpu_at(table.offset), staging_start, heap_type);
        return cc::unit{};
    };

    {
        auto r = fill(_ctx._descriptor_heap, _view_heap, _dx_layout->descriptor_count,
                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, _view_start, group->table, group->table_start);
        CC_RETURN_IF_ERROR(r);
    }
    {
        auto r = fill(_ctx._sampler_heap, _sampler_heap, _dx_layout->sampler_descriptor_count,
                      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, _sampler_start, group->sampler_table,
                      group->sampler_table_start);
        CC_RETURN_IF_ERROR(r);
    }

    // Hand the group the resource references the staged descriptors point at, in the two shapes it needs:
    // a scalar binding is auto-tracked through the hazard vectors, an array binding is declared per dispatch.
    for (auto const& s : _dx_layout->view_slots)
    {
        if (s.binding.is_array())
        {
            auto ab = dx12_array_binding{.name = s.binding.name,
                                         .is_texture = sg::shape_of(s.binding.type) == sg::view_shape::texture,
                                         .elements = {}};
            for (int e = 0; e < int(s.binding.count); ++e)
            {
                auto const& res = _resources[s.table_offset + e];
                ab.elements.push_back({.buffer = res.buffer, .texture = res.texture, .range = res.range});
            }
            group->array_bindings.push_back(cc::move(ab));
            continue;
        }

        auto const& res = _resources[s.table_offset];
        if (res.texture != nullptr)
        {
            group->referenced_textures.push_back(res.texture);
            group->texture_hazard_views.push_back({res.texture, res.range, res.access});
        }
        else if (res.buffer != nullptr)
        {
            group->referenced.push_back(res.buffer);
            group->hazard_views.push_back({res.buffer, res.access});
        }
        // else: a scalar slot with no resource — the null acceleration structure, which tracks nothing.
    }

    return sg::binding_group_handle(cc::move(group));
}
} // namespace sg::backend::dx12
