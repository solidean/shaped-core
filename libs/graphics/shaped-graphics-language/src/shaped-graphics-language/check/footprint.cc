#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/footprint.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

namespace sgl::check
{
namespace
{
/// What the code does to each slot, gathered touch by touch.
struct footprint_builder
{
    checked_module const& m;
    cc::vector<slot_footprint> slots;

    void touch(symbol_id binding, i32 member, bool reads, bool writes)
    {
        for (auto& s : slots)
            if (s.binding == binding && s.member == member)
            {
                s.reads |= reads;
                s.writes |= writes;
                return;
            }
        slots.push_back({.binding = binding, .member = member, .host_name = {}, .reads = reads, .writes = writes});
    }

    /// A binding member standing in the tree, reached with `reads` / `writes`: a resource names its own slot, and a
    /// plain member is its binding's constant block.
    void touch_member(flat_binding_member const& b, bool reads, bool writes)
    {
        auto const& info = m.bindings[m.at(b.binding).info];
        // an `@inline` block is set rather than bound, and workgroup memory has no host side at all
        if (info.is_inline || info.is_workgroup)
            return;
        auto const& member = m.at(info.members)[b.member];
        auto const kind = m.at(member.type).kind;
        if (kind == type_kind::sampler)
            return;
        touch(b.binding, m.takes_slots(member.type) ? b.member : -1, reads, writes);
    }
};

/// Whether every expression of `e` has exactly one parent, which `legalize` guarantees by compacting what it returns.
/// It is what lets one pass over the arrays stand for the tree: nothing in them is unreachable, and nothing is shared.
[[maybe_unused]] bool has_one_parent_each(flat_entry_point const& e)
{
    auto parents = cc::vector<i32>::create_defaulted(e.exprs.size());
    auto const count = [&](flat_expr_id id)
    {
        if (impl::is_known(e, id))
            ++parents[index_of(id)];
    };
    for (auto const& s : e.stmts)
    {
        impl::for_each_expr_of(s, count);
        impl::for_each_pattern_of(e, s,
                                  [&](ast::range_of<flat_expr_id> patterns)
                                  {
                                      if (impl::is_known(e, patterns))
                                          for (auto const p : e.at(patterns))
                                              count(p);
                                  });
    }
    for (auto const& x : e.exprs)
        impl::for_each_operand(e, x, count);
    for (auto const p : parents)
        if (p != 1)
            return false;
    return true;
}
} // namespace

cc::vector<slot_footprint> footprint_of(checked_module const& m, flat_entry_point const& e)
{
    CC_ASSERT(has_one_parent_each(e), "footprint_of needs a legalized entry point, whose every node has one parent");

    // A binding member is read where it stands unless its parent says otherwise, and the two parents that do claim it
    // first: a store names the buffer it writes, and a builtin's parameter says how a resource handed to it is used.
    auto b = footprint_builder{.m = m, .slots = {}};
    auto claimed = cc::vector<bool>::create_defaulted(e.exprs.size());

    for (auto const& s : e.stmts)
    {
        auto const* const assign = s.node.try_as<flat_assign>();
        if (assign == nullptr)
            continue;
        // Every index on the way to the place is an ordinary read, which the scan below finds.
        auto place = assign->place;
        for (auto steps = isize(0); impl::is_known(e, place) && steps < e.exprs.size(); ++steps)
        {
            auto const& x = e.at(place);
            if (auto const* const member = x.node.try_as<flat_member>())
            {
                place = member->object;
                continue;
            }
            // an array element is part of its local, and its index an ordinary read
            if (auto const* const element = x.node.try_as<flat_element>())
            {
                place = element->object;
                continue;
            }
            // workgroup memory is no slot of the host's
            if (auto const* const b = x.node.try_as<flat_binding_member>(); b != nullptr && b->is_workgroup)
                break;
            if (auto const* const element = x.node.try_as<flat_buffer_element>())
            {
                // the buffer, or the binding array it is an element of
                auto named = element->buffer;
                if (auto const* const array = impl::is_known(e, named) ? e.at(named).node.try_as<flat_element>() : nullptr)
                    named = array->object;
                auto const* const buffer
                    = impl::is_known(e, named) ? e.at(named).node.try_as<flat_binding_member>() : nullptr;
                CC_ASSERT(buffer != nullptr,
                          "a stored buffer element names its buffer as a binding member; a footprint "
                          "that met anything else would miss the write");
                b.touch_member(*buffer, false, true);
                claimed[index_of(named)] = true;
                break;
            }
            CC_ASSERT(x.node.is<flat_local_ref>(), "an assignment's place is a local, a member of one, or a buffer "
                                                   "element; a new kind of place must teach the footprint its write");
            break;
        }
    }

    for (auto const& x : e.exprs)
    {
        auto const* const call = x.node.try_as<flat_call>();
        if (call == nullptr || !impl::is_known(e, call->arguments))
            continue;
        // An `out` image is written, a `mut` one read and written, anything else read.
        auto const& fn = m.functions[m.at(call->callee).info];
        auto const parameters = m.at(fn.parameters);
        auto const arguments = e.at(call->arguments);
        for (auto i = isize(0); i < arguments.size() && i < parameters.size(); ++i)
        {
            if (!impl::is_known(e, arguments[i]))
                continue;
            // an atomic update writes the buffer whose element it names
            if (auto const* const element = e.at(arguments[i]).node.try_as<flat_buffer_element>();
                element != nullptr && m.at(parameters[i].type).kind == type_kind::atomic
                && impl::is_known(e, element->buffer))
            {
                auto named = element->buffer;
                if (auto const* const array = e.at(named).node.try_as<flat_element>())
                    named = array->object;
                if (auto const* const buffer = e.at(named).node.try_as<flat_binding_member>())
                {
                    b.touch_member(*buffer, true, true);
                    claimed[index_of(named)] = true;
                }
                continue;
            }
            // an element of a binding array is used as its member is, whichever element it is
            auto named = arguments[i];
            if (auto const* const element = e.at(named).node.try_as<flat_element>();
                element != nullptr && impl::is_known(e, element->object))
                named = element->object;
            auto const* const member = e.at(named).node.try_as<flat_binding_member>();
            if (member == nullptr)
                continue;
            auto const& type = m.at(parameters[i].type);
            b.touch_member(*member, type.access != access_mode::write, type.access != access_mode::read || type.is_mut);
            claimed[index_of(named)] = true;
        }
    }

    for (auto i = isize(0); i < e.exprs.size(); ++i)
        if (auto const* const member = e.exprs[i].node.try_as<flat_binding_member>(); member != nullptr && !claimed[i])
            b.touch_member(*member, true, false);

    // In the order of the binding list and then of the members, so a footprint reads the way its bindings are written.
    auto ordered = cc::vector<slot_footprint>();
    for (auto const binding : e.bindings)
    {
        auto const& symbol = m.at(binding);
        auto const members = m.at(m.bindings[symbol.info].members);
        for (auto member = i32(-1); member < i32(members.size()); ++member)
            for (auto const& s : b.slots)
                if (s.binding == binding && s.member == member)
                {
                    auto slot = s;
                    if (member < 0)
                    {
                        slot.host_name = symbol.name;
                        slot.view = slot_view::constants;
                    }
                    else
                    {
                        // a binding array's view is its element's
                        auto element = members[member].type;
                        while (m.at(element).kind == type_kind::array)
                            element = m.at(element).element;
                        auto const& type = m.at(element);
                        slot.host_name = cc::format("{}.{}", symbol.name, members[member].name);
                        slot.view
                            = type.kind == type_kind::image || type.is_mut ? slot_view::storage : slot_view::read_only;
                    }
                    ordered.push_back(cc::move(slot));
                }
    }
    return ordered;
}

cc::string footprint_text(cc::span<slot_footprint const> footprint)
{
    auto text = cc::string();
    for (auto const& slot : footprint)
    {
        if (!text.empty())
            text += ", ";
        text += slot.host_name;
        text += slot.reads && slot.writes ? ": read write" : slot.writes ? ": write" : ": read";
    }
    return text;
}
} // namespace sgl::check
