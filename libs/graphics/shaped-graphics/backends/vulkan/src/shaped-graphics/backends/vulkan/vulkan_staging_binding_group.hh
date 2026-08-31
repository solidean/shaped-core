#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/fwd.hh>

/// vulkan staging_binding_group: the group's descriptors held as plain host memory, laid out exactly as one set is.
///
/// This is where the descriptor-buffer choice pays for itself.
/// A descriptor there is bytes rather than an object, so the staging image is byte-identical to what a minted group
/// holds — and `mint` is one memcpy of the whole set, whatever its size.
/// dx12 reaches the same place through CopyDescriptorsSimple out of a non-shader-visible heap; a descriptor *pool*
/// backend could not, since it would have to re-write every binding into a freshly allocated set.
///
/// **Descriptor positions are byte offsets, but the base addresses descriptors by index.**
/// So this keeps a flattened numbering — every binding's elements contiguous, in declaration order — and resolves each
/// flat index to its byte offset through `_offsets`.
/// The base only ever adds an element index to a binding's first descriptor, which that numbering preserves.
///
/// A minted group is an ordinary persistent vulkan_binding_group — same bind path, same epoch-deferred free of its
/// range — so nothing downstream knows a snapshot from a directly-created group.
class sg::backend::vulkan::vulkan_staging_binding_group final : public sg::staging_binding_group
{
public:
    [[nodiscard]] static cc::result<vulkan_staging_binding_group_handle> create(vulkan_context& ctx,
                                                                                vulkan_binding_group_layout_handle layout);

    /// `descriptor_offsets` is the base's slot map — see staging_binding_group's constructor.
    /// Only `create` builds one correctly; it is not a public entry point.
    vulkan_staging_binding_group(vulkan_context& ctx,
                                 vulkan_binding_group_layout_handle layout,
                                 cc::vector<int> descriptor_offsets,
                                 cc::vector<isize> byte_offsets);
    ~vulkan_staging_binding_group() override;

protected:
    void write_view_descriptors(int first_descriptor, sg::binding const& b, cc::span<sg::raw_view const> views) override;
    void clear_view_descriptors(int first_descriptor, sg::binding const& b, int count) override;
    void write_sampler_descriptor(int descriptor_index, sg::sampler const& smp) override;
    [[nodiscard]] cc::result<sg::binding_group_handle> mint() override;

private:
    /// What one staged descriptor references, in the shape a vulkan_binding_group wants back.
    /// At most one of buffer / texture is set; both null is a vacant descriptor.
    struct staged
    {
        vulkan_buffer_handle buffer;
        vulkan_texture_handle texture;
        sg::subresource_range range;
        sg::view_class access = sg::view_class::readonly;
    };

    /// Fills every descriptor with its binding's empty value, which the base requires before it hands the group out.
    [[nodiscard]] cc::result<cc::unit> initialize();

    [[nodiscard]] byte* at(int flat_descriptor) { return _image.data() + _offsets[flat_descriptor]; }

    vulkan_context& _ctx;
    vulkan_binding_group_layout_handle _vk_layout;

    cc::vector<byte> _image;       ///< one layout-sized set — the CPU-side truth a snapshot copies
    cc::vector<isize> _offsets;    ///< byte offset of each flattened descriptor within the image
    cc::vector<staged> _resources; ///< what each flattened descriptor references, indexed alike
};
