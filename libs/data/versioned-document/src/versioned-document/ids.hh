#pragma once

#include <clean-core/common/hash.hh>
#include <clean-core/string/interned_string.hh>
#include <clean-core/string/string_view.hh>
#include <versioned-document/fwd.hh>

#include <compare>

/// The three id types: entity, component type and property.
///
/// Each wraps a cc::interned_string, and each is a distinct type, so the three cannot be swapped at a call site.
/// A property path is exactly one of each, and it is the addressable unit of the whole system.
///
/// **Interning is process-local.** Never serialize an id or hash durable data by it — as_string_view() is what
/// everything durable commits to, and two runs agree on nothing else.
///
/// **Ordering is by canonical bytes, always.** cc::interned_string offers a second, cheaper order over the
/// process-local identity, and it is deliberately not reachable from here: an id order decides how assignments are
/// sorted, that sort feeds the op hash, and a hash that depends on the run is not a content address.
///
/// The design is [the concept](../../docs/concepts/the-model.md#entity-ids-are-strings).

namespace vdoc::impl
{
/// The shared body of the three id types.
///
/// Derived is the concrete id, so entity_id and property_id inherit from different bases and are unrelated types.
/// Nothing here is virtual and the layout is one pointer, so an id stays as cheap to pass as the handle inside it.
template <class Derived>
struct interned_id
{
    /// The empty id, which is valid and is what a default-constructed id is.
    /// An empty entity name is a name like any other, and storage attaches no meaning to it.
    constexpr interned_id() = default;

    explicit constexpr interned_id(cc::interned_string name) : _name(name) {}

    /// Interns the bytes in the process-wide table.
    [[nodiscard]] static Derived of(cc::string_view name) { return Derived(cc::intern(name)); }

    /// The canonical bytes — the only form of an id that may be written down.
    [[nodiscard]] cc::string_view as_string_view() const { return _name.as_string_view(); }
    [[nodiscard]] cc::interned_string as_interned_string() const { return _name; }

    [[nodiscard]] isize size() const { return _name.size(); }
    [[nodiscard]] bool empty() const { return _name.empty(); }

    /// Sound as a pointer compare, since one table hands out one entry per distinct byte sequence.
    [[nodiscard]] friend constexpr bool operator==(Derived lhs, Derived rhs) { return lhs._name == rhs._name; }
    [[nodiscard]] friend constexpr bool operator!=(Derived lhs, Derived rhs) { return lhs._name != rhs._name; }

    /// Orders by the canonical bytes, so every process that interns the same names agrees.
    /// Costs a memcmp — the one operation on an id that is not a pointer operation.
    [[nodiscard]] std::strong_ordering compare_bytes(Derived rhs) const { return _name.compare_bytes(rhs._name); }

    /// Sort predicate over the canonical bytes.
    struct by_bytes
    {
        [[nodiscard]] bool operator()(Derived lhs, Derived rhs) const { return lhs.compare_bytes(rhs) < 0; }
    };

    /// The hash of the canonical bytes, so an id finds a string-keyed entry and two runs agree.
    [[nodiscard]] friend u64 hash(Derived v) { return hash(v._name); }

private:
    cc::interned_string _name;
};
} // namespace vdoc::impl

/// Names an entity — an arbitrary application-chosen string, whether a name, a path or a uuid.
/// The library attaches no structure to it whatsoever.
struct vdoc::entity_id : impl::interned_id<entity_id>
{
    using impl::interned_id<entity_id>::interned_id;
};

/// Names a component type, e.g. "Transform".
/// Names starting with `$` are reserved for the library — see [reserved names](../../docs/concepts/interpretation.md#reserved-names).
struct vdoc::component_type_id : impl::interned_id<component_type_id>
{
    using impl::interned_id<component_type_id>::interned_id;
};

/// Names a property within a component, e.g. "position".
/// `$`-prefixed names are reserved here too.
struct vdoc::property_id : impl::interned_id<property_id>
{
    using impl::interned_id<property_id>::interned_id;
};

/// One property path: the addressable unit of the whole system.
/// Ops assign to paths, conflicts are per path, and diffs are lists of paths — nothing smaller is ever addressed.
struct vdoc::property_path
{
    entity_id entity;
    component_type_id component;
    property_id property;

    [[nodiscard]] friend constexpr bool operator==(property_path const&, property_path const&) = default;

    /// The order an op's assignments are canonicalized into, by entity then component then property, all by bytes.
    /// This feeds the op hash, so it is a FORMAT CONSTANT rather than a convenience.
    [[nodiscard]] std::strong_ordering compare_bytes(property_path const& rhs) const
    {
        if (auto const by_entity = entity.compare_bytes(rhs.entity); by_entity != 0)
            return by_entity;
        if (auto const by_component = component.compare_bytes(rhs.component); by_component != 0)
            return by_component;
        return property.compare_bytes(rhs.property);
    }

    struct by_bytes
    {
        [[nodiscard]] bool operator()(property_path const& lhs, property_path const& rhs) const
        {
            return lhs.compare_bytes(rhs) < 0;
        }
    };

    [[nodiscard]] friend u64 hash(property_path const& v)
    {
        return cc::make_hash(hash(v.entity), hash(v.component), hash(v.property));
    }
};
