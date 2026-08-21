#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/fwd.hh>

/// dx12 staging_binding_group: the group's descriptors held in a private **non-shader-visible** heap, one per layout slot.
/// A set writes exactly one descriptor there — the cost of changing one binding no longer scales with the table's size.
/// `mint` allocates a persistent range in the context's shader-visible heap and fills it with a single
/// `CopyDescriptorsSimple`, which is why a 4096-element bindless table is affordable to re-snapshot.
///
/// The staging heap is per-group rather than sub-allocated: it is plain CPU memory with exactly one owner, and it lives
/// as long as the staging group does.
///
/// Descriptor positions come from the base, which resolved and bounds-checked them against the layout, so this class
/// only writes descriptors and mirrors what they reference.
///
/// A minted group is an ordinary persistent dx12_binding_group — same bind path, same epoch-deferred free of its range,
/// same `declare_array_*_access` resolution — so nothing downstream knows a snapshot from a directly-created group.
class sg::backend::dx12::dx12_staging_binding_group final : public sg::staging_binding_group
{
public:
    [[nodiscard]] static cc::result<dx12_staging_binding_group_handle> create(dx12_context& ctx,
                                                                              dx12_binding_group_layout_handle layout);

    /// `descriptor_offsets` is the base's slot map — see staging_binding_group's constructor.
    /// Only `create` builds one correctly; it is not a public entry point.
    dx12_staging_binding_group(dx12_context& ctx,
                               dx12_binding_group_layout_handle layout,
                               cc::vector<int> descriptor_offsets);
    ~dx12_staging_binding_group() override;

protected:
    void write_view_descriptors(int first_descriptor, sg::binding const& b, cc::span<sg::raw_view const> views) override;
    void clear_view_descriptors(int first_descriptor, sg::binding const& b, int count) override;
    void write_sampler_descriptor(int descriptor_index, sg::sampler const& smp) override;
    [[nodiscard]] cc::result<sg::binding_group_handle> mint() override;

private:
    /// What one staged descriptor references, in the shapes a dx12_binding_group wants back.
    /// At most one of buffer / texture is set; both null is a vacant descriptor.
    /// An acceleration-structure binding stores the TLAS's storage buffer, which is what the trace actually reads.
    struct staged
    {
        dx12_buffer_handle buffer;
        dx12_texture_handle texture;
        sg::subresource_range range;
        sg::view_class access = sg::view_class::readonly;
    };

    [[nodiscard]] cc::result<cc::unit> initialize();

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE view_cpu_at(int index) const
    {
        return {_view_start.ptr + SIZE_T(index) * SIZE_T(_view_increment)};
    }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_at(int index) const
    {
        return {_sampler_start.ptr + SIZE_T(index) * SIZE_T(_sampler_increment)};
    }

    dx12_context& _ctx;
    dx12_binding_group_layout_handle _dx_layout;

    ComPtr<ID3D12DescriptorHeap> _view_heap; // CBV/SRV/UAV staging image, one descriptor per table slot
    ComPtr<ID3D12DescriptorHeap> _sampler_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE _view_start = {};
    D3D12_CPU_DESCRIPTOR_HANDLE _sampler_start = {};
    int _view_increment = 0;
    int _sampler_increment = 0;

    cc::vector<staged> _resources; // one per CBV/SRV/UAV descriptor, indexed as the descriptors are
};
