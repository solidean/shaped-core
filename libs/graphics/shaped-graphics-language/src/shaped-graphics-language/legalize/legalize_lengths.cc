#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

// CHK-491: an unbounded binding array's length is sg's, which it writes at each dispatch or draw from the groups bound,
// into the reserved `sg_array_lengths`; dx12 and vulkan read it there, metal from the array's own table.

using namespace sgl;
using namespace sgl::check;

namespace
{
/// Whether `x` is the length of an unbounded binding array, rather than of a buffer.
bool is_array_length(checked_module const& m, flat_entry_point const& e, flat_expr const& x)
{
    auto const* const length = x.node.try_as<flat_buffer_length>();
    if (length == nullptr || !is_valid(length->buffer) || index_of(length->buffer) >= e.exprs.size())
        return false;
    auto const& t = m.at(e.at(length->buffer).type);
    return t.kind == type_kind::array && t.count == 0;
}

/// The position of member `member` of `binding` among the entry point's unbounded binding arrays, in list order,
/// which is the order sg writes their lengths in: a group holds one at most.
i32 position_of(checked_module const& m, flat_entry_point const& e, symbol_id binding, i32 member)
{
    auto k = 0;
    for (auto const id : e.bindings)
    {
        auto const members = m.at(m.bindings[m.at(id).info].members);
        for (auto i = isize(0); i < members.size(); ++i)
        {
            auto const& t = m.at(members[i].type);
            if (t.kind != type_kind::array || t.count != 0)
                continue;
            if (id == binding && i == member)
                return k;
            ++k;
        }
    }
    return -1;
}
} // namespace

bool sgl::check::reads_array_lengths(checked_module const& m, flat_entry_point const& e)
{
    for (auto const& x : e.exprs)
        if (is_array_length(m, e, x))
            return true;
    return false;
}

flat_entry_point sgl::check::with_reserved_array_lengths(checked_module const& m, flat_entry_point const& e)
{
    auto const binding = impl::reserved_symbol_of(m, "sg_array_lengths");
    // a test's own prelude declares no lengths, and its tree is left as it is
    if (!is_valid(binding))
        return e;
    auto out = flat_builder::extend(m, e);
    auto const values_type = m.at(m.bindings[m.at(binding).info].members)[0].type;
    auto const vector_type = m.at(values_type).element;
    auto const count = out.e.exprs.size();
    for (auto i = isize(0); i < count; ++i)
    {
        if (!is_array_length(m, out.e, out.e.exprs[i]))
            continue;
        auto const& length = out.e.exprs[i].node.as<flat_buffer_length>();
        auto const* const named = out.e.at(length.buffer).node.try_as<flat_binding_member>();
        CC_ASSERT(named != nullptr, "an unbounded array is named only as its binding's member (CHK-491)");
        auto const k = position_of(m, out.e, named->binding, named->member);
        CC_ASSERT(k >= 0, "an unbounded array whose length is read is one of the entry point's bindings");
        // `int(sg_array_lengths.values[k / 4][k % 4])`, sg writing 16 lengths as `uint4[4]`
        auto const values = out.add_expr(values_type, flat_binding_member{.binding = binding, .member = 0});
        auto const vector = out.add_expr(vector_type, flat_element{.object = values, .index = out.int_literal(k / 4)});
        auto const scalar
            = out.add_expr(out.type_named(builtins::k_uint), flat_member{.object = vector, .member = k % 4});
        flat_expr_id const arguments[] = {scalar};
        auto const converted = out.call("convert_uint_to_int", arguments);
        out.e.exprs[i].node = out.e.exprs[index_of(converted)].node;
    }
    auto is_listed = false;
    for (auto const id : out.e.reserved)
        is_listed = is_listed || id == binding;
    if (!is_listed)
        out.e.reserved.push_back(binding);
    return cc::move(out.e);
}
