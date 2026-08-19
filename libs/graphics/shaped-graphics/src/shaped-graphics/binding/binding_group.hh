#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>

/// A binding name paired with the views bound to it — the input to create_binding_group.
/// A scalar binding (count == 1) takes exactly one view; an array binding (count > 1) takes exactly `count`, one per element.
/// A vacant array element is a null-handle view: it must still carry the binding's arm (a `raw_texture_view` with a null
/// texture keeps its dimension + format, so the backend builds a matching null descriptor; a null-buffer view likewise).
/// A typed view converts implicitly to its `raw_view`, so call sites read `{"Output", {buf->as_readwrite_buffer<u32>()}}`.
struct sg::named_view
{
    cc::string name;
    cc::vector<raw_view> views;
};

/// A binding name paired with a sampler state.
/// As a `create_binding_group_layout` argument it declares a *static* sampler, baked into the pipeline layout's root signature.
/// As a `create_binding_group` argument it supplies a *dynamic* sampler for a sampler binding of that name.
/// Same value type either way.
struct sg::named_sampler
{
    cc::string name;
    sg::sampler sampler; // qualified: bare `sampler` here would shadow the type (GCC -Wchanges-meaning)
};

/// A binding_group_layout instantiated with concrete resources bound: each named view is matched to a layout binding, validated, and turned into a backend descriptor.
/// Bound at a pipeline-layout slot as a unit.
/// Immutable after creation — rebind by recreating.
/// Held via binding_group_handle.
///
/// Abstract: a backend subclasses it and owns the native allocation (dx12 descriptor-heap range,
/// vulkan VkDescriptorSet). See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
class sg::binding_group
{
public:
    virtual ~binding_group();

protected:
    binding_group() = default;
};
