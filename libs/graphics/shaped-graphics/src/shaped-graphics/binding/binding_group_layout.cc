#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>

namespace sg
{
binding_group_layout::~binding_group_layout() = default;
} // namespace sg

namespace
{
cc::string names_of(sg::binding_group_layout const* layout)
{
    if (layout == nullptr)
        return cc::string("no layout");
    auto out = cc::string("[");
    for (auto const& b : layout->bindings())
        out.appendf("{}{}", out.size() == 1 ? "" : ", ", b.name);
    out += "]";
    return out;
}
} // namespace

cc::string sg::impl::describe_layout_mismatch(int slot,
                                              binding_group_layout const* expected,
                                              binding_group_layout const* bound)
{
    return cc::format("slot {} of the bound pipeline layout holds a group of {}, and the group bound there was created "
                      "against a different layout, one of {}; a group fits a slot only if it was created against the "
                      "very layout the slot holds",
                      slot, names_of(expected), names_of(bound));
}
