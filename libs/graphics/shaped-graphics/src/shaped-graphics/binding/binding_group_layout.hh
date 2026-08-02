#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// The frozen schema of one bindable resource group/set: built from a shader's `binding`s, composed into
/// a pipeline_layout, and instantiated by binding_groups. Held via binding_group_layout_handle.
///
/// Abstract: a backend subclasses it and owns the native object (dx12 descriptor-table schema, vulkan
/// VkDescriptorSetLayout). See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
class binding_group_layout
{
public:
    virtual ~binding_group_layout();

protected:
    binding_group_layout() = default;
};
} // namespace sg
