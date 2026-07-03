#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/views.hh>

namespace sg
{
/// A binding name paired with the view bound to it — the input to create_binding_group. A typed view
/// converts implicitly to its `raw_view`, so call sites read `{"Output", buf->as_readwrite_buffer<u32>()}`.
struct named_view
{
    cc::string name;
    raw_view view;
};

/// A binding_layout instance with concrete resources bound: each named view is matched to a layout
/// binding, validated, and turned into a backend descriptor. Bound to a pipeline as a unit. Immutable
/// after creation (rebind by recreating). Held via binding_group_handle.
///
/// Abstract: a backend subclasses it and owns the native allocation (dx12 descriptor-heap range,
/// vulkan VkDescriptorSet). See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
class binding_group
{
public:
    virtual ~binding_group();

protected:
    binding_group() = default;
};
} // namespace sg
