#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/function_ref.hh>
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

/// One member of a `@vertex struct` as the polyfill reads it: its stream's slot and its words there.
struct vertex_member
{
    i32 stream = 0;
    bool is_per_instance = false;
    /// Its first word in an element of its stream.
    i32 offset = 0;
    /// One word a scalar leaf, or one word an element for a packed `@format`.
    i32 words = 0;
    cc::string format;
};

/// How a `@vertex struct` lies in its buffers: the host packs each stream's members in order, every leaf 4 bytes.
struct vertex_layout
{
    cc::vector<vertex_member> members;
    /// The words an element of each stream holds, in slot order.
    cc::vector<i32> strides;
};

/// How `input`, a `@vertex struct`, lies in the draw's buffers, or why the polyfill cannot read it yet.
[[nodiscard]] cc::result<vertex_layout, cc::string> vertex_layout_of(checked_module const& m, type_id input);

/// Copies a core tree into another one, every local, label and message its own there.
/// What `on_return` writes stands where a `return` stood.
struct grafter
{
    flat_entry_point const& in;
    flat_builder& out;
    cc::function_ref<void(flat_expr_id value, stmt_list& into)> on_return;

    cc::vector<local_id> locals;
    cc::vector<label_id> labels;
    i32 first_message = 0;
    u32 first_call_site = 0;

    void prepare()
    {
        for (auto const& local : in.locals)
        {
            auto const id = out.add_local(local.kind, local.name, local.type);
            auto& copy = out.e.locals[index_of(id)];
            copy.is_mut = local.is_mut;
            copy.is_undefined = local.is_undefined;
            copy.is_zeroed = local.is_zeroed;
            locals.push_back(id);
        }
        for (auto const& label : in.labels)
        {
            auto const id = out.add_label(label.name);
            out.e.labels[index_of(id)].is_rolled = label.is_rolled;
            labels.push_back(id);
        }
        first_message = i32(out.e.messages.size());
        for (auto const& message : in.messages)
            out.e.messages.push_back(message);
        first_call_site = u32(out.e.call_sites.size());
        for (auto const& site : in.call_sites)
            out.e.call_sites.push_back(site);
    }

    [[nodiscard]] local_id local(local_id id) const { return is_valid(id) ? locals[index_of(id)] : id; }
    [[nodiscard]] label_id label(label_id id) const { return is_valid(id) ? labels[index_of(id)] : id; }
    [[nodiscard]] ast::range_of<call_site> sites(ast::range_of<call_site> r) const
    {
        if (r.count > 0)
            r.first = decltype(r.first)(u32(r.first) + first_call_site);
        return r;
    }

    flat_expr_id expr(flat_expr_id id)
    {
        if (!is_valid(id))
            return id;
        auto copy = in.at(id);
        copy.inlined_through = sites(copy.inlined_through);
        copy.node.visit([&](flat_local_ref& n) { n.local = local(n.local); },
                        [&](flat_member& n) { n.object = expr(n.object); },
                        [&](flat_buffer_element& n)
                        {
                            n.buffer = expr(n.buffer);
                            n.index = expr(n.index);
                        },
                        [&](flat_buffer_length& n) { n.buffer = expr(n.buffer); },
                        [&](flat_written_word& n) { n.word = expr(n.word); },
                        [&](flat_element& n)
                        {
                            n.object = expr(n.object);
                            n.index = expr(n.index);
                        },
                        [&](flat_construct& n) { n.arguments = exprs(n.arguments); }, [&](flat_call& n)
                        { n.arguments = exprs(n.arguments); }, [&](flat_not& n) { n.operand = expr(n.operand); },
                        [&](flat_and& n)
                        {
                            n.lhs = expr(n.lhs);
                            n.rhs = expr(n.rhs);
                        },
                        [&](flat_or& n)
                        {
                            n.lhs = expr(n.lhs);
                            n.rhs = expr(n.rhs);
                        },
                        [&](flat_block& n)
                        {
                            n.label = label(n.label);
                            n.body = body(n.body);
                        },
                        [&](flat_by_target& n)
                        {
                            n.native = expr(n.native);
                            n.emulated = expr(n.emulated);
                        },
                        [&](auto&) {});
        out.e.exprs.push_back(cc::move(copy));
        return flat_expr_id(out.e.exprs.size() - 1);
    }

    ast::range_of<flat_expr_id> exprs(ast::range_of<flat_expr_id> range)
    {
        auto list = cc::vector<flat_expr_id>();
        for (auto const id : in.at(range))
            list.push_back(expr(id));
        return out.expr_list(list);
    }

    ast::range_of<flat_arm> arms(ast::range_of<flat_arm> range)
    {
        auto list = cc::vector<flat_arm>();
        for (auto const& arm : in.at(range))
            list.push_back({.patterns = exprs(arm.patterns), .body = body(arm.body)});
        return out.arm_list(list);
    }

    ast::range_of<flat_stmt_id> body(ast::range_of<flat_stmt_id> range) { return out.stmt_list(statements(range)); }

    stmt_list statements(ast::range_of<flat_stmt_id> range)
    {
        auto copies = stmt_list();
        for (auto const id : in.at(range))
        {
            auto copy = in.at(id);
            copy.inlined_through = sites(copy.inlined_through);
            if (auto const* const r = copy.node.try_as<flat_return>())
            {
                out.from = copy.from;
                out.inlined_through = copy.inlined_through;
                on_return(expr(r->value), copies);
                continue;
            }
            copy.node.visit(
                [&](flat_let& n)
                {
                    n.local = local(n.local);
                    n.value = expr(n.value);
                },
                [&](flat_var& n)
                {
                    n.local = local(n.local);
                    n.value = expr(n.value);
                },
                [&](flat_assign& n)
                {
                    n.place = expr(n.place);
                    n.value = expr(n.value);
                },
                [&](flat_print& n)
                {
                    if (n.message >= 0)
                        n.message += first_message;
                    n.values = exprs(n.values);
                },
                [&](flat_eval& n) { n.value = expr(n.value); },
                [&](flat_if& n)
                {
                    n.condition = expr(n.condition);
                    n.then_body = body(n.then_body);
                    n.else_body = body(n.else_body);
                },
                [&](flat_block& n)
                {
                    n.label = label(n.label);
                    n.body = body(n.body);
                },
                [&](flat_leave& n)
                {
                    n.target = label(n.target);
                    n.value = expr(n.value);
                },
                [&](flat_loop& n)
                {
                    n.label = label(n.label);
                    n.body = body(n.body);
                },
                [&](flat_while& n)
                {
                    n.label = label(n.label);
                    n.condition = expr(n.condition);
                    n.body = body(n.body);
                },
                [&](flat_for& n)
                {
                    n.label = label(n.label);
                    n.index = local(n.index);
                    n.first = expr(n.first);
                    n.end = expr(n.end);
                    n.body = body(n.body);
                },
                [&](flat_continue& n) { n.target = label(n.target); }, [&](flat_once& n) { n.body = body(n.body); },
                [&](flat_case& n)
                {
                    n.scrutinee = expr(n.scrutinee);
                    n.arms = arms(n.arms);
                    n.default_body = body(n.default_body);
                },
                [&](flat_switch& n)
                {
                    n.scrutinee = expr(n.scrutinee);
                    n.arms = arms(n.arms);
                    n.default_body = body(n.default_body);
                },
                [&](flat_check& n) { n.body = body(n.body); }, [&](auto&) {});
            out.e.stmts.push_back(cc::move(copy));
            copies.push_back(flat_stmt_id(out.e.stmts.size() - 1));
        }
        return copies;
    }
};

/// `a` and `b` merged, each entry once, in the order `a` then `b` first lists it.
template <class T>
void merge_into(cc::vector<T>& a, cc::span<T const> b)
{
    for (auto const& x : b)
    {
        auto has = false;
        for (auto const& y : a)
            has = has || y == x;
        if (!has)
            a.push_back(x);
    }
}

/// What `v`, the vertex stage of a polyfilled pipeline, makes of vertex `k` of the draw's range in `instance`: its index
/// read where the draw is indexed, its stage struct from the `sg_geometry_*` buffers, then the stage itself, grafted.
/// The result is a local of the stage's result type; `layout` is `v.input`'s, where it has one (CHK-525, CHK-527).
[[nodiscard]] cc::result<flat_expr_id, cc::string> run_vertex_stage(flat_builder& out,
                                                                    words& w,
                                                                    flat_entry_point const& v,
                                                                    cc::optional<vertex_layout> const& layout,
                                                                    flat_expr_id k,
                                                                    flat_expr_id instance,
                                                                    stmt_list& body);

/// What `e`, a compute form that runs the stage `v` inside it, takes of `v`'s header: bindings, reserved bindings,
/// options, traced rays, text types, features and float guarantee.
void merge_stage_into(flat_entry_point& e, flat_entry_point const& v);

/// The reserved binding `name`, listed in `out.e.reserved` once.
[[nodiscard]] symbol_id listed(flat_builder& out, cc::string_view name);

/// The local of stage input `input`, added as a parameter of `type` where the entry point reads none.
[[nodiscard]] local_id stage_input_of(flat_builder& out, stage_input input, cc::string_view desired, type_id type);

/// The workgroup `group` of a compute form dispatched in `workgroup`-sized groups, numbered `x + nx * (y + ny * z)`,
/// the grid being the threads sg wrote into `sg_dispatch_grid`; what it binds goes into `into`.
[[nodiscard]] flat_expr_id linear_workgroup(words& w,
                                            local_id group,
                                            cc::fixed_array<i32, 3> const& workgroup,
                                            stmt_list& into);

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
