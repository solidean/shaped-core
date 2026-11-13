#include "reserved_bindings.hh"

#include <shaped-graphics-language/emit/emit.hh>

using namespace sgl;

check::reserved_binding const* check::reserved_binding_named(cc::string_view name)
{
    for (auto const& b : k_reserved_bindings)
        if (b.name == name)
            return &b;
    return nullptr;
}

bool check::reaches_reserved(reserved_binding const& b, emit::target target, stage s, feature_set device)
{
    auto const& on = b.targets[u8(target)];
    if ((on.stages & stage_bit(s)) == 0)
        return false;
    return (on.stages_needing_feature & stage_bit(s)) == 0 || device.has(on.needs);
}
