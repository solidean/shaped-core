#include <clean-core/common/assertf.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

// CHK-524: a mesh stage on a target without one is a compute pass and a draw.
// The compute form writes each workgroup's counts, indices and vertices into fixed slots of `sg_mesh_output`, and the
// vertex form reads them back from `sg_mesh_input`, one invocation a corner of every primitive slot in order, so the
// draw keeps the primitive order the native stage has.

using namespace sgl;
using namespace sgl::check;

namespace
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

[[nodiscard]] cc::optional<leaf_kind> scalar_kind_of(checked_module const& m, type_id type)
{
    auto const name = m.name_of(type);
    if (name == builtins::k_float)
        return leaf_kind::float_;
    if (name == builtins::k_int)
        return leaf_kind::int_;
    if (name == builtins::k_uint)
        return leaf_kind::uint_;
    if (name == "bool")
        return leaf_kind::bool_;
    if (name == "half")
        return leaf_kind::half_;
    return {};
}

/// Every scalar leaf of `type` in field order, or the first type that is none.
[[nodiscard]] cc::result<cc::vector<leaf>, cc::string> leaves_of(checked_module const& m, type_id type)
{
    auto result = cc::vector<leaf>();
    auto failed = cc::string();
    auto const walk = [&](auto const& self, type_id t, cc::vector<i32>& path) -> void
    {
        if (!failed.empty())
            return;
        if (auto const kind = scalar_kind_of(m, t); kind.has_value())
        {
            result.push_back({.kind = kind.value(), .path = path});
            return;
        }
        auto const& info = m.at(t);
        if (info.kind == type_kind::structure && !info.is_opaque && info.members.count > 0)
        {
            auto const members = m.at(info.members);
            for (auto i = isize(0); i < members.size(); ++i)
            {
                path.push_back(i32(i));
                self(self, members[i].type, path);
                path.remove_back();
            }
            return;
        }
        if (info.kind == type_kind::array && info.count > 0)
        {
            for (auto i = 0; i < info.count; ++i)
            {
                path.push_back(i);
                self(self, info.element, path);
                path.remove_back();
            }
            return;
        }
        failed = cc::string(m.name_of(t));
    };
    auto path = cc::vector<i32>();
    walk(walk, type, path);
    if (!failed.empty())
        return cc::error(cc::move(failed));
    return result;
}

/// The mesh output parameter of `e`, whose type is `mesh_output[V]` or `mesh_output[V, P]`.
[[nodiscard]] local_id output_of(checked_module const& m, flat_entry_point const& e)
{
    for (auto i = isize(0); i < e.locals.size(); ++i)
    {
        auto const& local = e.locals[i];
        auto const& t = m.at(local.type);
        if (local.kind == local_kind::parameter && t.kind == type_kind::stream && t.count == 0)
            return local_id(i);
    }
    return local_id::none;
}

/// What a call of a mesh output's method writes: 0 for `set_counts`, 1 for `vertex`, 2 for `triangle`, 3 for
/// `primitive` and 4 for `line`; -1 for every other expression.
[[nodiscard]] i32 mesh_write_of(checked_module const& m, flat_expr const& x)
{
    auto const* const call = x.node.try_as<flat_call>();
    auto const* const record = call == nullptr ? nullptr : m.builtin_function(call->intrinsic);
    if (record == nullptr)
        return -1;
    constexpr cc::string_view names[] = {"set_counts", "vertex", "triangle", "primitive", "line"};
    for (auto i = 0; i < 5; ++i)
        if (record->name == names[i])
            return i;
    return -1;
}

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
    [[nodiscard]] flat_expr_id at(flat_expr_id index)
    {
        return out.buffer_element(out.binding_member(binding, 0), index);
    }
    [[nodiscard]] flat_expr_id at(flat_expr_id base, u32 offset)
    {
        return at(offset == 0 ? base : binary("add_uint", base, word(offset)));
    }
    /// A uint local bound to `value`, so it is computed once.
    [[nodiscard]] flat_expr_id bound(cc::string_view desired, flat_expr_id value, impl::stmt_list& into)
    {
        auto const local = out.add_local(local_kind::temporary, desired, out.e.at(value).type);
        into.push_back(out.let(local, value));
        return out.local(local);
    }

    [[nodiscard]] flat_expr_id leaf_of(flat_expr_id value, leaf const& l)
    {
        auto x = value;
        for (auto const step : l.path)
        {
            auto const& t = out.m.at(out.e.at(x).type);
            x = t.kind == type_kind::array
                  ? out.add_expr(t.element, flat_element{.object = x, .index = out.int_literal(step)})
                  : out.member(x, step);
        }
        return x;
    }

    [[nodiscard]] flat_expr_id encode(flat_expr_id lane, leaf_kind kind)
    {
        switch (kind)
        {
        case leaf_kind::uint_:
            return lane;
        case leaf_kind::int_:
            return unary("convert_int_to_uint", lane);
        case leaf_kind::bool_:
            return unary("convert_bool_to_uint", lane);
        case leaf_kind::half_:
            return unary("reinterpret_as_uint", unary("convert_half_to_float", lane));
        case leaf_kind::float_:
            return unary("reinterpret_as_uint", lane);
        }
        return lane;
    }

    [[nodiscard]] flat_expr_id decode(flat_expr_id w, leaf_kind kind)
    {
        switch (kind)
        {
        case leaf_kind::uint_:
            return w;
        case leaf_kind::int_:
            return unary("reinterpret_as_int", w);
        case leaf_kind::bool_:
            return binary("not_equal_uint", w, word(0));
        case leaf_kind::half_:
            return unary("convert_float_to_half", unary("reinterpret_as_float", w));
        case leaf_kind::float_:
            return unary("reinterpret_as_float", w);
        }
        return w;
    }

    /// The value of `type` whose leaves stand in order from word `first`, built bottom-up.
    [[nodiscard]] flat_expr_id read_value(type_id type, flat_expr_id first, cc::span<leaf const> leaves, isize& next)
    {
        if (auto const kind = scalar_kind_of(out.m, type); kind.has_value())
            return decode(at(first, u32(next++)), kind.value());
        auto const& info = out.m.at(type);
        auto parts = cc::vector<flat_expr_id>();
        if (info.kind == type_kind::array)
        {
            for (auto i = 0; i < info.count; ++i)
                parts.push_back(read_value(info.element, first, leaves, next));
            return out.construct(type, parts);
        }
        for (auto const& member : out.m.at(info.members))
            parts.push_back(read_value(member.type, first, leaves, next));
        return out.construct(type, parts);
    }
};

/// Rewrites every statement list under `range`, replacing each statement `replace` answers for.
template <class Replace>
ast::range_of<flat_stmt_id> rewritten(flat_builder& out, ast::range_of<flat_stmt_id> range, Replace&& replace)
{
    auto list = impl::stmt_list();
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

[[nodiscard]] symbol_id listed(flat_builder& out, cc::string_view name)
{
    auto const binding = impl::reserved_symbol_of(out.m, name);
    CC_ASSERTF(is_valid(binding), "the prelude declares the reserved '{}' (CHK-524)", name);
    auto is_listed = false;
    for (auto const id : out.e.reserved)
        is_listed = is_listed || id == binding;
    if (!is_listed)
        out.e.reserved.push_back(binding);
    return binding;
}

/// Every parameter of `out.e` but those `keep` names becomes a temporary nothing declares.
void drop_parameters(flat_builder& out, local_id keep = local_id::none)
{
    for (auto i = isize(0); i < out.e.locals.size(); ++i)
        if (out.e.locals[i].kind == local_kind::parameter && local_id(i) != keep)
            out.e.locals[i].kind = local_kind::temporary;
}
} // namespace

cc::result<mesh_polyfill_layout, cc::string> sgl::check::mesh_polyfill_layout_of(checked_module const& m,
                                                                                 flat_entry_point const& e)
{
    CC_ASSERT(e.entry_stage == stage::mesh, "only a mesh entry point has a polyfill layout");
    auto const output = output_of(m, e);
    if (!is_valid(output))
        return cc::error(cc::format("{} takes no mesh output", e.name));
    auto const& t = m.at(e.at(output).type);
    if (is_valid(t.primitive))
        return cc::error(cc::format("{} writes values per primitive, which the polyfill does not carry yet", e.name));
    for (auto i = isize(0); i < e.locals.size(); ++i)
    {
        auto is_input = false;
        for (auto const& input : e.stage_inputs)
            is_input = is_input || index_of(input.local) == i;
        if (e.locals[i].kind == local_kind::parameter && local_id(i) != output && !is_input)
            return cc::error(cc::format("{} takes a task stage's payload, which the polyfill does not carry yet", e.name));
    }
    auto const leaves = leaves_of(m, t.element);
    if (leaves.has_error())
        return cc::error(cc::format("{}'s vertex holds a '{}', which no word holds", e.name, leaves.error()));

    auto const& info = m.functions[m.at(e.function).info];
    auto layout = mesh_polyfill_layout{.corners = info.is_line_mesh ? 2 : 3,
                                       .max_vertices = info.max_vertices,
                                       .max_primitives = info.max_primitives,
                                       .vertex_words = i32(leaves.value().size())};
    layout.group_words = layout.first_vertex_word() + layout.max_vertices * layout.vertex_words;
    return layout;
}

flat_entry_point sgl::check::mesh_as_compute(checked_module const& m, flat_entry_point const& e)
{
    auto const layout = mesh_polyfill_layout_of(m, e);
    CC_ASSERTF(layout.has_value(), "{}", layout.has_error() ? layout.error() : cc::string());
    auto const output = output_of(m, e);
    auto const vertex_type = m.at(e.at(output).type).element;
    auto const leaves = leaves_of(m, vertex_type).value();

    auto out = flat_builder::extend(m, e);
    auto w = words{.out = out, .binding = listed(out, "sg_mesh_output")};
    auto const grid = listed(out, "sg_dispatch_grid");

    auto group = local_id::none;
    for (auto const& input : out.e.stage_inputs)
        if (input.input == stage_input::workgroup_id)
            group = input.local;
    if (!is_valid(group))
    {
        group = out.add_local(local_kind::parameter, "workgroup_id", out.type_named("int3"));
        out.e.stage_inputs.push_back({.input = stage_input::workgroup_id, .local = group});
    }

    // `base = (x + nx * (y + ny * z)) * group_words`, the grid being the threads sg dispatched
    auto prologue = impl::stmt_list();
    auto const lane = [&](flat_expr_id v, i32 axis) { return out.member(v, axis); };
    auto const id = [&](i32 axis) { return w.unary("convert_int_to_uint", lane(out.local(group), axis)); };
    auto const threads = out.binding_member(grid, 0);
    auto const nx = w.bound("mesh_nx", w.binary("divide_uint", lane(threads, 0), w.word(u32(e.workgroup[0]))), prologue);
    auto const ny = w.bound(
        "mesh_ny", w.binary("divide_uint", lane(out.binding_member(grid, 0), 1), w.word(u32(e.workgroup[1]))), prologue);
    auto const linear
        = w.binary("add_uint", id(0),
                   w.binary("multiply_uint", nx, w.binary("add_uint", id(1), w.binary("multiply_uint", ny, id(2)))));
    auto const base
        = w.bound("mesh_base", w.binary("multiply_uint", linear, w.word(u32(layout.value().group_words))), prologue);

    auto const replace = [&](flat_stmt_id sid, impl::stmt_list& into) -> bool
    {
        auto const* const eval = out.e.at(sid).node.try_as<flat_eval>();
        if (eval == nullptr || !is_valid(eval->value))
            return false;
        auto const write = mesh_write_of(m, out.e.at(eval->value));
        if (write < 0)
            return false;
        auto const& s = out.e.at(sid);
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        auto const arguments = cc::vector<flat_expr_id>::create_copy_of(
            cc::span<flat_expr_id const>(out.e.at(out.e.at(eval->value).node.as<flat_call>().arguments)));
        auto const store = [&](flat_expr_id first, u32 offset, flat_expr_id value)
        { into.push_back(out.assign(w.at(first, offset), value)); };
        if (write == 0)
        {
            store(base, 0, arguments[1]);
            store(base, 1, arguments[2]);
            return true;
        }
        auto const corners = u32(layout.value().corners);
        if (write == 2 || write == 4)
        {
            auto const indices = w.bound("mesh_indices", arguments[2], into);
            auto const first = w.bound("mesh_at",
                                       w.binary("add_uint", base,
                                                w.binary("add_uint", w.word(u32(layout.value().first_index_word())),
                                                         w.binary("multiply_uint", arguments[1], w.word(corners)))),
                                       into);
            for (auto c = u32(0); c < corners; ++c)
                store(first, c, lane(indices, i32(c)));
            return true;
        }
        // a vertex: its leaves in field order
        auto const value = w.bound("mesh_vertex", arguments[2], into);
        auto const first = w.bound(
            "mesh_at",
            w.binary("add_uint", base,
                     w.binary("add_uint", w.word(u32(layout.value().first_vertex_word())),
                              w.binary("multiply_uint", arguments[1], w.word(u32(layout.value().vertex_words))))),
            into);
        for (auto k = isize(0); k < leaves.size(); ++k)
            store(first, u32(k), w.encode(w.leaf_of(value, leaves[k]), leaves[k].kind));
        return true;
    };

    auto body = impl::stmt_list::create_copy_of(cc::span<flat_stmt_id const>(prologue));
    auto const rest = rewritten(out, out.e.body, replace);
    for (auto const s : out.e.at(rest))
        body.push_back(s);
    out.e.locals[index_of(output)].kind = local_kind::temporary;
    // an output that is the first parameter stands as the entry point's stage struct, which a compute stage has none of
    if (out.e.input == out.e.at(output).type)
        out.e.input = type_id::none;
    out.e.entry_stage = stage::compute;
    out.e.features.remove(feature::mesh_shader);
    // the slots' base and the grid are read by every store, which the compaction gives a node each
    return impl::compacted(m, out.e, body);
}

flat_entry_point sgl::check::mesh_as_vertex_fetch(checked_module const& m, flat_entry_point const& e)
{
    auto const layout = mesh_polyfill_layout_of(m, e);
    CC_ASSERTF(layout.has_value(), "{}", layout.has_error() ? layout.error() : cc::string());
    auto const& l = layout.value();
    auto const output = output_of(m, e);
    auto const vertex_type = m.at(e.at(output).type).element;
    auto const leaves = leaves_of(m, vertex_type).value();

    auto out = flat_builder::extend(m, e);
    drop_parameters(out);
    out.e.stage_inputs.clear();
    out.e.bindings.clear();
    out.e.reserved.clear();
    out.e.traced_rays.clear();
    out.e.debug_sites.clear();
    out.e.options.clear();
    out.e.legalized = nullptr;
    out.e.input = type_id::none;
    out.e.workgroup = {1, 1, 1};
    out.e.written_words = 0;
    out.e.zeroes_workgroup = false;
    out.e.unshadowed_bytes = 0;
    out.e.workgroup_index = local_id::none;
    out.e.entry_stage = stage::vertex;
    out.e.result = vertex_type;
    out.e.features = {};
    for (auto const& leaf : leaves)
        if (leaf.kind == leaf_kind::half_)
            out.e.features.set(feature::shader_f16);
    if (e.body.count > 0)
        out.from = e.at(e.at(e.body)[0]).from;
    out.inlined_through = {};

    auto w = words{.out = out, .binding = listed(out, "sg_mesh_input")};
    auto const vertex_index = out.add_local(local_kind::parameter, "vertex_index", out.type_named(builtins::k_int));
    out.e.stage_inputs.push_back({.input = stage_input::vertex_index, .local = vertex_index});

    // `vertex_index` is a corner of primitive slot `p`, which is slot `i` of workgroup `g`
    auto body = impl::stmt_list();
    auto const corners = w.word(u32(l.corners));
    auto const primitives = w.word(u32(l.max_primitives));
    auto const v = w.bound("mesh_corner_of", w.unary("convert_int_to_uint", out.local(vertex_index)), body);
    auto const p = w.bound("mesh_slot", w.binary("divide_uint", v, corners), body);
    auto const base = w.bound(
        "mesh_base", w.binary("multiply_uint", w.binary("divide_uint", p, primitives), w.word(u32(l.group_words))), body);
    auto const i = w.binary("remainder_uint", p, primitives);
    auto const c = w.binary("remainder_uint", v, corners);
    auto const corner
        = w.bound("mesh_index",
                  out.buffer_element(out.binding_member(w.binding, 0),
                                     w.binary("add_uint", base,
                                              w.binary("add_uint", w.word(u32(l.first_index_word())),
                                                       w.binary("add_uint", w.binary("multiply_uint", i, corners), c)))),
                  body);
    auto const used = w.bound("mesh_used",
                              out.and_(w.binary("less_uint", w.binary("remainder_uint", p, primitives), w.at(base, 1)),
                                       out.and_(w.binary("less_uint", corner, w.at(base, 0)),
                                                w.binary("less_uint", corner, w.word(u32(l.max_vertices))))),
                              body);

    // an unused slot reads vertex 0, which its workgroup's slots always hold, and stands outside the clip volume
    auto const read_from = out.add_local(local_kind::temporary, "mesh_read", w.uint_type());
    body.push_back(out.var(read_from, w.word(0)));
    flat_stmt_id const take[] = {out.assign(out.local(read_from), corner)};
    body.push_back(out.if_(used, take));
    auto const first
        = w.bound("mesh_at",
                  w.binary("add_uint", base,
                           w.binary("add_uint", w.word(u32(l.first_vertex_word())),
                                    w.binary("multiply_uint", out.local(read_from), w.word(u32(l.vertex_words))))),
                  body);
    auto next = isize(0);
    auto const result = out.add_local(local_kind::temporary, "mesh_vertex", vertex_type);
    body.push_back(out.var(result, w.read_value(vertex_type, first, leaves, next)));

    auto const members = m.at(m.at(vertex_type).members);
    for (auto k = isize(0); k < members.size(); ++k)
    {
        if (!members[k].is_position)
            continue;
        flat_expr_id const outside[] = {out.literal(2.0), out.literal(2.0), out.literal(2.0), out.literal(1.0)};
        flat_stmt_id const clipped[]
            = {out.assign(out.member(out.local(result), i32(k)), out.construct(members[k].type, outside))};
        body.push_back(out.if_(out.not_(used), clipped));
    }
    body.push_back(out.return_(out.local(result)));
    return impl::compacted(m, out.e, body);
}
