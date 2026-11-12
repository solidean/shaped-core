#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics-language/check/flat_builder.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

// What the stage polyfills share: a value as words of a reserved buffer, and the draw that reads a workgroup's slots
// back (CHK-524, CHK-525).

namespace sgl::check::impl
{
/// A scalar leaf of a value, which one word holds: its bits, a bool as 0 or 1, and a `half` widened to a float.
enum class leaf_kind : u8
{
    float_,
    int_,
    uint_,
    bool_,
    half_,
};

struct leaf
{
    leaf_kind kind = leaf_kind::float_;
    /// The member positions from the value to this leaf, an array's element as its index.
    cc::vector<i32> path;
};

[[nodiscard]] cc::optional<leaf_kind> scalar_kind_of(checked_module const& m, type_id type);

/// Every scalar leaf of `type` in field order, or the first type that is none.
[[nodiscard]] cc::result<cc::vector<leaf>, cc::string> leaves_of(checked_module const& m, type_id type);

/// Writes and reads one value's leaves as words of a reserved word buffer.
struct words
{
    flat_builder& out;
    symbol_id binding = symbol_id::none;

    [[nodiscard]] type_id uint_type() const { return out.type_named(builtins::k_uint); }
    [[nodiscard]] flat_expr_id word(u32 value)
    {
        return out.add_expr(uint_type(), flat_int_literal{.value = i32(value), .is_unsigned = true});
    }
    [[nodiscard]] flat_expr_id binary(cc::string_view name, flat_expr_id a, flat_expr_id b)
    {
        flat_expr_id const arguments[] = {a, b};
        return out.call(name, arguments);
    }
    [[nodiscard]] flat_expr_id unary(cc::string_view name, flat_expr_id a)
    {
        flat_expr_id const arguments[] = {a};
        return out.call(name, arguments);
    }
    [[nodiscard]] flat_expr_id at(flat_expr_id index) { return at_of(binding, index); }
    [[nodiscard]] flat_expr_id at(flat_expr_id base, u32 offset)
    {
        return at(offset == 0 ? base : binary("add_uint", base, word(offset)));
    }
    /// Word `index` of the reserved word buffer `b`, which need not be `binding`.
    [[nodiscard]] flat_expr_id at_of(symbol_id b, flat_expr_id index)
    {
        return out.buffer_element(out.binding_member(b, 0), index);
    }
    /// A local bound to `value`, so it is computed once.
    [[nodiscard]] flat_expr_id bound(cc::string_view desired, flat_expr_id value, stmt_list& into)
    {
        auto const local = out.add_local(local_kind::temporary, desired, out.e.at(value).type);
        into.push_back(out.let(local, value));
        return out.local(local);
    }

    [[nodiscard]] flat_expr_id leaf_of(flat_expr_id value, leaf const& l);
    [[nodiscard]] flat_expr_id encode(flat_expr_id lane, leaf_kind kind);
    [[nodiscard]] flat_expr_id decode(flat_expr_id w, leaf_kind kind);

    /// The value of `type` whose leaves stand in order from word `first`, built bottom-up.
    [[nodiscard]] flat_expr_id read_value(type_id type, flat_expr_id first, isize& next);
};

/// Rewrites every statement list under `range`, replacing each statement `replace` answers for.
template <class Replace>
ast::range_of<flat_stmt_id> rewritten(flat_builder& out, ast::range_of<flat_stmt_id> range, Replace&& replace)
{
    auto list = stmt_list();
    auto const ids = cc::vector<flat_stmt_id>::create_copy_of(cc::span<flat_stmt_id const>(out.e.at(range)));
    for (auto const id : ids)
    {
        if (replace(id, list))
            continue;
        auto node = out.e.at(id).node;
        auto const inner = [&](ast::range_of<flat_stmt_id> r) { return rewritten(out, r, replace); };
        if (auto* const i = node.try_as<flat_if>())
        {
            i->then_body = inner(i->then_body);
            i->else_body = inner(i->else_body);
        }
        else if (auto* const l = node.try_as<flat_loop>())
            l->body = inner(l->body);
        else if (auto* const w = node.try_as<flat_while>())
            w->body = inner(w->body);
        else if (auto* const f = node.try_as<flat_for>())
            f->body = inner(f->body);
        else if (auto* const o = node.try_as<flat_once>())
            o->body = inner(o->body);
        else if (auto* const b = node.try_as<flat_block>())
            b->body = inner(b->body);
        else if (auto* const sw = node.try_as<flat_switch>())
        {
            for (auto k = isize(0); k < isize(sw->arms.count); ++k)
            {
                auto const arm = isize(sw->arms.first) + k;
                out.e.arms[arm].body = inner(out.e.arms[arm].body);
            }
            sw->default_body = inner(sw->default_body);
        }
        out.e.stmts[index_of(id)].node = cc::move(node);
        list.push_back(id);
    }
    return out.stmt_list(list);
}

/// The reserved binding `name`, listed in `out.e.reserved` once.
[[nodiscard]] symbol_id listed(flat_builder& out, cc::string_view name);

/// The local of stage input `input`, added as a parameter of `type` where the entry point reads none.
[[nodiscard]] local_id stage_input_of(flat_builder& out, stage_input input, cc::string_view desired, type_id type);

/// The workgroup `group` of a compute form dispatched in `workgroup`-sized groups, numbered `x + nx * (y + ny * z)`,
/// the grid being the threads sg wrote into `sg_dispatch_grid`; what it binds goes into `into`.
/// `count`, where given, is set to the workgroups of the grid.
[[nodiscard]] flat_expr_id linear_workgroup(words& w,
                                            local_id group,
                                            cc::fixed_array<i32, 3> const& workgroup,
                                            stmt_list& into,
                                            flat_expr_id* count = nullptr);

/// An entry point of stage `s` with no parameter, binding or result, which takes `e`'s header and origin; its
/// locals stay, as temporaries, so a body built from `e`'s values reads them.
[[nodiscard]] flat_builder fresh_entry(checked_module const& m, flat_entry_point const& e, stage s);

/// The vertex entry point that draws a polyfill's slots back, one invocation a corner of every primitive slot, from
/// `sg_mesh_input`; the corners of a slot past its workgroup's count stand outside the clip volume.
/// `e` is the entry point the slots came from, whose header and origin it takes, and `vertex_type` the struct a slot holds.
/// Where the slots hold values per primitive, `corner_type` is `add_mesh_corner`'s struct of both, which it returns instead.
[[nodiscard]] flat_entry_point slots_as_vertex_fetch(checked_module const& m,
                                                     flat_entry_point const& e,
                                                     mesh_polyfill_layout const& l,
                                                     type_id vertex_type,
                                                     type_id corner_type = type_id::none);
} // namespace sgl::check::impl
