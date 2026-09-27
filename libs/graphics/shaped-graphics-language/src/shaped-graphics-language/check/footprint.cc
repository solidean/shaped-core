#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/footprint.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

namespace sgl::check
{
namespace
{
/// Walks the statements an entry point runs and records what each touches of its bindings.
struct footprint_walk
{
    checked_module const& m;
    flat_entry_point const& e;
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
        if (info.is_inline)
            return;
        auto const& member = m.at(info.members)[b.member];
        auto const kind = m.at(member.type).kind;
        if (kind == type_kind::sampler)
            return;
        touch(b.binding, is_resource(kind) ? b.member : -1, reads, writes);
    }

    /// `x` read as a value, and everything under it.
    void read(flat_expr_id id, int depth)
    {
        if (depth > impl::k_max_depth || !impl::is_known(e, id))
            return;
        auto const& x = e.at(id);

        if (auto const* const b = x.node.try_as<flat_binding_member>())
            return touch_member(*b, true, false);

        if (auto const* const call = x.node.try_as<flat_call>())
        {
            // A resource passed to a builtin is used as the parameter it lands in says: an `out` image is written, a
            // `mut` one read and written, anything else read.
            auto const& fn = m.functions[m.at(call->callee).info];
            auto const parameters = m.at(fn.parameters);
            auto const arguments = e.at(call->arguments);
            for (auto i = isize(0); i < arguments.size(); ++i)
            {
                auto const& argument = e.at(arguments[i]);
                auto const* const b = argument.node.try_as<flat_binding_member>();
                if (b == nullptr || i >= parameters.size())
                {
                    read(arguments[i], depth + 1);
                    continue;
                }
                auto const& type = m.at(parameters[i].type);
                auto const writes = type.access != access_mode::read || type.is_mut;
                auto const reads = type.access != access_mode::write;
                touch_member(*b, reads, writes);
            }
            return;
        }

        if (auto const* const block = x.node.try_as<flat_block>())
            return walk(block->body, depth + 1);

        impl::for_each_operand(e, x, [&](flat_expr_id operand) { read(operand, depth + 1); });
    }

    /// `place` assigned to: the buffer element it ends in is written, and every index on the way is read.
    void write(flat_expr_id place, int depth)
    {
        if (depth > impl::k_max_depth || !impl::is_known(e, place))
            return;
        auto const& x = e.at(place);
        if (auto const* const member = x.node.try_as<flat_member>())
            return write(member->object, depth + 1);
        if (auto const* const element = x.node.try_as<flat_buffer_element>())
        {
            if (auto const* const b = e.at(element->buffer).node.try_as<flat_binding_member>())
                touch_member(*b, false, true);
            else
                read(element->buffer, depth + 1);
            return read(element->index, depth + 1);
        }
        // A local: nothing of a binding is touched.
    }

    void walk(ast::range_of<flat_stmt_id> body, int depth)
    {
        if (depth > impl::k_max_depth || !impl::is_known(e, body))
            return;
        for (auto const id : e.at(body))
        {
            if (!impl::is_known(e, id))
                continue;
            auto const& s = e.at(id);
            if (auto const* const assign = s.node.try_as<flat_assign>())
            {
                write(assign->place, depth + 1);
                read(assign->value, depth + 1);
            }
            else
                impl::for_each_expr_of(s, [&](flat_expr_id x) { read(x, depth + 1); });
            impl::for_each_pattern_of(e, s,
                                      [&](ast::range_of<flat_expr_id> patterns)
                                      {
                                          if (impl::is_known(e, patterns))
                                              for (auto const p : e.at(patterns))
                                                  read(p, depth + 1);
                                      });
            impl::for_each_body_of(e, s, [&](ast::range_of<flat_stmt_id> inner) { walk(inner, depth + 1); });
        }
    }
};
} // namespace

cc::vector<slot_footprint> footprint_of(checked_module const& m, flat_entry_point const& e)
{
    auto w = footprint_walk{.m = m, .e = e, .slots = {}};
    w.walk(e.body, 0);

    // In the order of the binding list and then of the members, so a footprint reads the way its bindings are written.
    auto ordered = cc::vector<slot_footprint>();
    for (auto const binding : e.bindings)
    {
        auto const& symbol = m.at(binding);
        auto const members = m.at(m.bindings[symbol.info].members);
        for (auto member = i32(-1); member < i32(members.size()); ++member)
            for (auto const& s : w.slots)
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
                        auto const& type = m.at(members[member].type);
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
