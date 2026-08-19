#include "component.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <versioned-document/op_builder.hh>


using namespace cc::primitive_defines;

vdoc::property_id vdoc::reserved::schema_version()
{
    static auto const id = property_id::of("$schema_version");
    return id;
}

vdoc::property_id vdoc::reserved::alive()
{
    static auto const id = property_id::of("$alive");
    return id;
}

vdoc::component_type_id vdoc::reserved::entity()
{
    static auto const id = component_type_id::of("$entity");
    return id;
}

bool vdoc::reserved::is_reserved(cc::string_view name)
{
    return name.starts_with(sigil);
}

vdoc::component_writer vdoc::component_writer::create_for(op_builder& builder, entity_id entity, component_type_id component)
{
    auto w = component_writer();
    w._builder = &builder;
    w._entity = entity;
    w._component = component;
    return w;
}

void vdoc::component_writer::set(property_id property, value v)
{
    CC_ASSERT(_builder != nullptr, "component_writer was never bound");
    CC_ASSERT(!reserved::is_reserved(property.as_string_view()), "a component may not write a $-prefixed property");

    _builder->set_raw(_entity, _component, property, cc::move(v));
}

vdoc::component_schema const* vdoc::component_registry::try_get(component_type_id type) const
{
    auto lo = isize(0);
    auto hi = _schemas.size();
    while (lo < hi)
    {
        auto const mid = lo + (hi - lo) / 2;
        auto const order = _schemas[mid].type.compare_bytes(type);
        if (order == 0)
            return &_schemas[mid];

        if (order < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}

void vdoc::component_registry::add(component_schema schema)
{
    CC_ASSERT(!reserved::is_reserved(schema.type.as_string_view()), "a component type may not use the $ sigil");
    CC_ASSERT(!schema.type.empty(), "a component type needs a name");

    if (auto const* const existing = try_get(schema.type))
    {
        CC_ASSERT(*existing == schema, "two C++ types are registered under one component type name");
        return;
    }

    // Registration is rare and lookup is not, so the vector pays for staying sorted here.
    _schemas.push_back(cc::move(schema));
    cc::sort(_schemas,
             [](component_schema const& a, component_schema const& b) { return a.type.compare_bytes(b.type) < 0; });
}

void vdoc::component_registry::merge(component_registry const& other)
{
    for (auto const& s : other._schemas)
        add(s);
}
