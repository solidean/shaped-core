#include "parse_policy.hh"

#include <clean-core/common/assert.hh>

using namespace cc::primitive_defines;

vdoc::property_reader vdoc::property_reader::create_for(raw_component const& raw,
                                                        parse_policy const& policy,
                                                        parse_report& report,
                                                        entity_id entity,
                                                        component_type_id component,
                                                        i32 schema_version)
{
    auto r = property_reader();
    r._raw = &raw;
    r._policy = &policy;
    r._report = &report;
    r._entity = entity;
    r._component = component;
    r._schema_version = schema_version;
    return r;
}

cc::optional<vdoc::value_view> vdoc::property_reader::try_get(property_id property) const
{
    CC_ASSERT(_raw != nullptr && _policy != nullptr && _report != nullptr, "property_reader was never bound");

    auto const* const prop = _raw->try_get(property);
    if (prop == nullptr)
        return {};

    auto const writers = cc::span<property_value const>(prop->writers);
    CC_ASSERT(!writers.empty(), "a materialized property always has at least one writer");

    if (writers.size() == 1)
        return writers[0].value;

    // Byte equality is the whole test, because the value encoding is canonical.
    auto all_agree = true;
    for (auto i = isize(1); i < writers.size(); ++i)
        if (writers[i].value != writers[0].value)
        {
            all_agree = false;
            break;
        }

    auto const path = property_path{.entity = _entity, .component = _component, .property = property};

    if (all_agree)
    {
        _report->agreed_multi_values.push_back({.path = path, .writer_count = writers.size()});
        return writers[0].value;
    }

    return _policy->resolve_multi_value(path, writers, *_report);
}
