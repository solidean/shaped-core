#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <versioned-document/parse_policy.hh>
#include <versioned-document/value.hh>

#include <concepts>
#include <type_traits>

/// The component protocol: how an application says what a component is, and how the library carries that type-erased.
///
/// **The library ships zero components.**
/// Not one, not even a convenience name — the moment there is a built-in component, the library has an opinion about
/// what a document is for.
///
/// An application declares one by specializing `component_traits<C>`:
///
///     template <>
///     struct vdoc::component_traits<my_transform>
///     {
///         static constexpr cc::string_view type_name = "Transform";
///         static constexpr i32 schema_version = 1;
///         static void write(my_transform const& c, vdoc::component_writer& w);
///         static cc::optional<my_transform> parse(vdoc::property_reader const& r);
///     };
///
/// The design is [the concept](../../docs/concept.md#components-belong-to-the-application).

/// The `$`-prefixed names the library owns.
/// Applications must not use the sigil, for a component type or for a property.
namespace vdoc::reserved
{
/// The sigil marking a library-owned name.
inline constexpr char sigil = '$';

/// The schema version its writer stamped, on any component.
/// Absent means 0, which is a document written through set_raw rather than an unknown version.
[[nodiscard]] property_id schema_version();

/// Deletion, on any component; absent means alive.
[[nodiscard]] property_id alive();

/// The component type carrying entity-level `$alive`.
/// It has no C++ struct and never reaches an application.
[[nodiscard]] component_type_id entity();

[[nodiscard]] bool is_reserved(cc::string_view name);
} // namespace vdoc::reserved

namespace vdoc
{
/// How an application's component type reads and writes itself — specialize this per component.
/// The primary template is deliberately undefined, so using an undeclared component is a compile error and not a
/// silent default.
template <class ComponentT>
struct component_traits;
} // namespace vdoc

namespace vdoc::impl
{
/// The interned type id of C, cached per type, so a query costs a pointer compare rather than an intern.
template <class ComponentT>
[[nodiscard]] component_type_id component_type_of()
{
    static auto const id = component_type_id::of(component_traits<ComponentT>::type_name);
    return id;
}

/// A per-type address, so a column can tell that two C++ types were registered under one component name.
/// A distinct address per instantiation, and no RTTI.
template <class ComponentT>
[[nodiscard]] void const* component_type_key()
{
    static constexpr char key = 0;
    return &key;
}
} // namespace vdoc::impl

namespace vdoc
{
/// What a type must provide to be a component.
template <class ComponentT>
concept is_component = requires(ComponentT const& c, component_writer& w, property_reader const& r) {
    { component_traits<ComponentT>::type_name } -> std::convertible_to<cc::string_view>;
    { component_traits<ComponentT>::schema_version } -> std::convertible_to<i32>;
    { component_traits<ComponentT>::write(c, w) } -> std::same_as<void>;
    { component_traits<ComponentT>::parse(r) } -> std::same_as<cc::optional<ComponentT>>;
} && std::is_move_constructible_v<ComponentT> && std::is_destructible_v<ComponentT>;
} // namespace vdoc

/// The sink `component_traits<C>::write` writes into: one component, of one entity, on one op_builder.
///
/// The builder must outlive the writer, which it always does — `op_builder::set` makes one, hands it to write, and
/// drops it before returning.
class vdoc::component_writer
{
    // construction
public:
    component_writer() = default;

    [[nodiscard]] static component_writer create_for(op_builder& builder, entity_id entity, component_type_id component);

    // writing
public:
    /// Stages one property write on the bound path.
    ///
    /// A `$`-prefixed name asserts: reserved properties are the library's, `$schema_version` is stamped by
    /// `op_builder::set` and `$alive` goes through `op_builder::set_alive`.
    /// Writing the same property twice asserts, exactly as `op_builder::set_raw` does.
    void set(property_id property, value v);
    void set(cc::string_view property, value v) { set(property_id::of(property), cc::move(v)); }

    // queries
public:
    [[nodiscard]] entity_id entity() const { return _entity; }
    [[nodiscard]] component_type_id component() const { return _component; }

private:
    op_builder* _builder = nullptr;
    entity_id _entity;
    component_type_id _component;
};

/// What the registry knows about one component type: its name, its current version, and the type-erased entry points.
///
/// A plain struct of function pointers, so it copies freely — which is what lets a document keep its own copy and stop
/// depending on the registry that built it.
struct vdoc::component_schema
{
    component_type_id type;
    i32 current_version = 0;

    /// The C++ type this was built from, for the mismatch assert in `document::get`.
    void const* type_key = nullptr;

    /// The layout of one component, for a document's type-erased column.
    isize component_size = 0;
    isize component_align = 0;

    /// Parses one component into `out_slot`, uninitialized storage of `component_size` bytes at `component_align`.
    /// Returns false when the component is dropped, in which case nothing was constructed.
    cc::function_ptr<bool(byte* out_slot, property_reader const& reader)> parse_into = nullptr;

    /// Destroys `count` adjacent components starting at `data`.
    cc::function_ptr<void(byte* data, isize count)> destroy_range = nullptr;

    /// Writes one live component through a writer.
    /// `op_builder::set` calls the traits directly; this is for code that only has a type id.
    cc::function_ptr<void(byte const* component, component_writer& writer)> write = nullptr;

    [[nodiscard]] friend bool operator==(component_schema const&, component_schema const&) = default;
};

namespace vdoc::impl
{
template <class ComponentT>
bool parse_component_into(byte* out_slot, property_reader const& reader)
{
    auto parsed = component_traits<ComponentT>::parse(reader);
    if (!parsed.has_value())
        return false;

    new (cc::placement_new, reinterpret_cast<ComponentT*>(out_slot)) ComponentT(cc::move(parsed.value()));
    return true;
}

template <class ComponentT>
void destroy_components(byte* data, isize count)
{
    auto* const components = reinterpret_cast<ComponentT*>(data);
    for (auto i = count; i > 0; --i)
        components[i - 1].~ComponentT();
}

template <class ComponentT>
void write_component(byte const* component, component_writer& writer)
{
    component_traits<ComponentT>::write(*reinterpret_cast<ComponentT const*>(component), writer);
}

template <class ComponentT>
[[nodiscard]] component_schema make_component_schema()
{
    return component_schema{.type = component_type_of<ComponentT>(),
                            .current_version = component_traits<ComponentT>::schema_version,
                            .type_key = component_type_key<ComponentT>(),
                            .component_size = sizeof(ComponentT),
                            .component_align = alignof(ComponentT),
                            .parse_into = &parse_component_into<ComponentT>,
                            .destroy_range = &destroy_components<ComponentT>,
                            .write = &write_component<ComponentT>};
}
} // namespace vdoc::impl

/// The set of component types an application understands, type-erased and runtime.
///
/// Extensible at any time, and a test may register a subset — the parser is driven entirely by what it is handed, and
/// never by what happens to be compiled in.
class vdoc::component_registry
{
    // registration
public:
    /// Registering the same type twice is idempotent.
    /// Two different C++ types under one `type_name` asserts, as does a `$`-prefixed name.
    template <class ComponentT>
    void register_component()
    {
        static_assert(is_component<ComponentT>, "specialize vdoc::component_traits<C> first - see "
                                                "docs/concept.md#components-belong-to-the-application");
        add(impl::make_component_schema<ComponentT>());
    }

    /// Adds every type of `other`; a type present in both must carry the identical schema.
    void merge(component_registry const& other);

    // queries
public:
    [[nodiscard]] component_schema const* try_get(component_type_id type) const;
    [[nodiscard]] bool contains(component_type_id type) const { return try_get(type) != nullptr; }
    [[nodiscard]] isize size() const { return _schemas.size(); }

    /// Every schema, sorted by component type id bytes.
    [[nodiscard]] cc::span<component_schema const> schemas() const { return _schemas; }

private:
    void add(component_schema schema);

    /// Sorted by component type id bytes, so lookup is a binary search and iteration is reproducible.
    cc::vector<component_schema> _schemas;
};
