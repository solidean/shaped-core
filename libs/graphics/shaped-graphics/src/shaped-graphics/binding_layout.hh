#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// The frozen schema of one bindable resource set: built from a shader's `binding`s, referenced by a
/// compute_pipeline, and instantiated by binding_groups. Held via binding_layout_handle.
///
/// Abstract: a backend subclasses it and owns the native object (dx12 root signature, vulkan
/// VkDescriptorSetLayout). See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
class binding_layout
{
public:
    virtual ~binding_layout();

protected:
    binding_layout() = default;
};
} // namespace sg
