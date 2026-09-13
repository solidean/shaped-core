#pragma once

#include <clean-core/common/utility.hh> // cc::move / cc::forward
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/binding.hh>
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

/// A layout slot paired with what is bound to it — the index-keyed twin of `named_view`.
///
/// Named for what it carries rather than for what it is: `sg::binding` is a *declaration* and `named_view` a
/// *supply*, so calling this `indexed_binding` would read as a `binding` carrying an index, which every
/// `binding` already has — and `binding::index` is the register number, a different integer from the slot.
///
/// The reason to key by slot is a caller that already knows it, which today means a generated binding group:
/// the same parse that produced the shader's address produced the slot, so `create` has no name to rediscover.
/// The string compare it saves is real but small — the groups built per frame in this tree carry one to four
/// bindings each.
///
/// A slot from the wrong layout is in range, wrong and silent, where a wrong *name* is an error message.
/// So a caller holding slots owes it to itself to know which layout they came from, and the cheapest way is to
/// acquire that layout from the same place the slots came from — which is what a generated group does.
/// Where the layout arrives from elsewhere, `binding_group_layout::structural_hash` is what compares the two.
///
/// `create_binding_group` is overloaded on which of the two a call passes, so a bare `{}` for "no views" is
/// ambiguous and has to name the span type it means.
struct sg::slotted_view
{
    binding_slot slot = binding_slot::invalid;
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

namespace sg
{
/// What a generated binding-group struct provides — the protocol slib's package generator emits, and the
/// constraint on every `<G>` scope template that takes one.
///
/// A group struct is a plain aggregate of bound resources plus this: the group index the shader's attribute
/// gave, the declarations the pass wrote the shader's own addresses from, and `gather`, which turns the fields
/// into the slot-keyed supply `create_binding_group` takes.
/// Everything a caller does with one — acquire its layout, create it, bind it — is a scope method constrained
/// on this concept, so the generator emits data and never an API of its own.
///
/// `declared_bindings` is the whole table rather than a stage's reflected subset, which is the property the
/// binding pass exists to buy: a merge over three stages' reflected bindings can silently omit a stage, and a
/// declaration cannot.
template <class G>
concept declared_binding_group
    = requires(G const& g, cc::vector<slotted_view>& views, cc::vector<named_sampler>& samplers) {
          requires std::is_same_v<std::remove_cv_t<decltype(G::group_index)>, int>;
          requires std::is_convertible_v<decltype(G::declared_bindings()), cc::span<binding const>>;
          requires std::is_convertible_v<decltype(G::declared_samplers()), cc::span<named_sampler const>>;
          g.gather(views, samplers);
      };
} // namespace sg

namespace sg::impl
{
/// `declared` first, then only those of `supplied` naming a sampler `declared` does not — so the shader wins.
///
/// Supplying a sampler the shader already declared `static` is a mistake rather than an override: a static
/// sampler is baked into the pipeline layout's root signature, so the supplied state would simply not take
/// effect.
/// It is dropped with an assertion, which is what names the mistake in a checked build.
[[nodiscard]] cc::vector<named_sampler> merge_declared_samplers(cc::span<named_sampler const> declared,
                                                                cc::span<named_sampler const> supplied);
} // namespace sg::impl

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
