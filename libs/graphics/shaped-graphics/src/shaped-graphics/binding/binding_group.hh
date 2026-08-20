#pragma once

#include <clean-core/common/utility.hh> // cc::move / cc::forward
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>

#include <type_traits>

/// The views bound to one binding name: ONE view for a scalar binding, a vector for an array binding.
/// A single view stores inline — no allocation — and any typed view converts implicitly, so the scalar
/// spelling stays `.view = buf->as_readwrite_buffer<u32>()`.
/// A wrapper rather than the bare variant because cc::variant's converting constructor is deliberately
/// exact: the typed-view → raw_view conversion has to happen here, in the templated constructor.
struct sg::bound_view
{
    cc::variant<raw_view, cc::vector<raw_view>> storage;

    /// No views — what a group creation rejects; fill it, or hand the aggregate a view directly.
    bound_view() : storage(cc::vector<raw_view>()) {}

    /// One view, from anything that converts to a raw_view — a typed view, a raw arm, or sg::vacant_view.
    template <class View>
        requires(std::is_convertible_v<View, raw_view>)
    bound_view(View&& view) : storage(raw_view(cc::forward<View>(view)))
    {
    }

    /// One view per array element, in element order.
    bound_view(cc::vector<raw_view> views) : storage(cc::move(views)) {}

    /// The bound views as one flat list, whichever arm is active.
    /// A span into this object — it must outlive the span.
    [[nodiscard]] cc::span<raw_view const> span() const
    {
        return storage.visit([](raw_view const& v) { return cc::span<raw_view const>(&v, 1); },
                             [](cc::vector<raw_view> const& vs) { return cc::span<raw_view const>(vs); });
    }

    [[nodiscard]] isize size() const { return span().size(); }
};

/// A binding name paired with what is bound to it — the input to create_binding_group.
/// A scalar binding (count == 1) takes exactly one view; an array binding (count > 1) takes exactly `count`, one per element.
/// A vacant array element is `sg::vacant_view` — the backend synthesizes its null descriptor from the binding.
/// A typed view converts implicitly, so call sites read `{.name = "Output", .view = buf->as_readwrite_buffer<u32>()}`.
struct sg::named_view
{
    cc::string name;
    bound_view view;
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
