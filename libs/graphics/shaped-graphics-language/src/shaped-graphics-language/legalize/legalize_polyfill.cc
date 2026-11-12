#include <clean-core/common/assertf.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/legalize/impl/polyfill.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

cc::optional<leaf_kind> sgl::check::impl::scalar_kind_of(checked_module const& m, type_id type)
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

cc::result<cc::vector<leaf>, cc::string> sgl::check::impl::leaves_of(checked_module const& m, type_id type)
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

flat_expr_id sgl::check::impl::words::leaf_of(flat_expr_id value, leaf const& l)
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

flat_expr_id sgl::check::impl::words::encode(flat_expr_id lane, leaf_kind kind)
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

flat_expr_id sgl::check::impl::words::decode(flat_expr_id w, leaf_kind kind)
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

flat_expr_id sgl::check::impl::words::read_value(type_id type, flat_expr_id first, isize& next)
{
    if (auto const kind = scalar_kind_of(out.m, type); kind.has_value())
        return decode(at(first, u32(next++)), kind.value());
    auto const& info = out.m.at(type);
    auto parts = cc::vector<flat_expr_id>();
    if (info.kind == type_kind::array)
    {
        for (auto i = 0; i < info.count; ++i)
            parts.push_back(read_value(info.element, first, next));
        return out.construct(type, parts);
    }
    for (auto const& member : out.m.at(info.members))
        parts.push_back(read_value(member.type, first, next));
    return out.construct(type, parts);
}

symbol_id sgl::check::impl::listed(flat_builder& out, cc::string_view name)
{
    auto const binding = reserved_symbol_of(out.m, name);
    CC_ASSERTF(is_valid(binding), "the prelude declares the reserved '{}'", name);
    auto is_listed = false;
    for (auto const id : out.e.reserved)
        is_listed = is_listed || id == binding;
    if (!is_listed)
        out.e.reserved.push_back(binding);
    return binding;
}

flat_entry_point sgl::check::impl::slots_as_vertex_fetch(checked_module const& m,
                                                         flat_entry_point const& e,
                                                         mesh_polyfill_layout const& l,
                                                         type_id vertex_type,
                                                         type_id corner_type)
{
    auto const result_type = is_valid(corner_type) ? corner_type : vertex_type;

    auto out = fresh_entry(m, e, stage::vertex);
    out.e.result = result_type;
    for (auto const& leaf : leaves_of(m, result_type).value())
        if (leaf.kind == leaf_kind::half_)
            out.e.features.set(feature::shader_f16);

    auto w = words{.out = out, .binding = listed(out, "sg_mesh_input")};
    auto const vertex_index = out.add_local(local_kind::parameter, "vertex_index", out.type_named(builtins::k_int));
    out.e.stage_inputs.push_back({.input = stage_input::vertex_index, .local = vertex_index});

    // `vertex_index` is a corner of primitive slot `p`, which is slot `i` of workgroup `g`
    auto body = stmt_list();
    auto const corners = w.word(u32(l.corners));
    auto const primitives = w.word(u32(l.max_primitives));
    auto const v = w.bound("mesh_corner_of", w.unary("convert_int_to_uint", out.local(vertex_index)), body);
    auto const p = w.bound("mesh_slot", w.binary("divide_uint", v, corners), body);
    auto const base = w.bound(
        "mesh_base", w.binary("multiply_uint", w.binary("divide_uint", p, primitives), w.word(u32(l.group_words))), body);
    auto const i = w.bound("mesh_slot_index", w.binary("remainder_uint", p, primitives), body);
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
    auto const result = out.add_local(local_kind::temporary, "mesh_vertex", result_type);
    if (is_valid(corner_type))
    {
        // the vertex's members from its slot, then the primitive's from the slot of the primitive this corner is of
        auto const primitive_at
            = w.bound("mesh_primitive_at",
                      w.binary("add_uint", base,
                               w.binary("add_uint", w.word(u32(l.first_primitive_word())),
                                        w.binary("multiply_uint", i, w.word(u32(l.primitive_words))))),
                      body);
        auto const vertex_members = isize(m.at(vertex_type).members.count);
        auto parts = cc::vector<flat_expr_id>();
        auto primitive_next = isize(0);
        for (auto const& member : m.at(m.at(corner_type).members))
            parts.push_back(isize(parts.size()) < vertex_members
                                ? w.read_value(member.type, first, next)
                                : w.read_value(member.type, primitive_at, primitive_next));
        body.push_back(out.var(result, out.construct(corner_type, parts)));
    }
    else
        body.push_back(out.var(result, w.read_value(vertex_type, first, next)));

    auto const members = m.at(m.at(result_type).members);
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
    return compacted(m, out.e, body);
}

flat_builder sgl::check::impl::fresh_entry(checked_module const& m, flat_entry_point const& e, stage s)
{
    auto out = flat_builder::extend(m, e);
    for (auto& local : out.e.locals)
        if (local.kind == local_kind::parameter)
            local.kind = local_kind::temporary;
    out.e.stage_inputs.clear();
    out.e.bindings.clear();
    out.e.reserved.clear();
    out.e.traced_rays.clear();
    out.e.debug_sites.clear();
    out.e.options.clear();
    out.e.legalized = nullptr;
    out.e.input = type_id::none;
    out.e.result = checked_module::void_type;
    out.e.workgroup = {1, 1, 1};
    out.e.is_auto_workgroup = false;
    out.e.written_words = 0;
    out.e.zeroes_workgroup = false;
    out.e.unshadowed_bytes = 0;
    out.e.workgroup_index = local_id::none;
    out.e.entry_stage = s;
    out.e.features = {};
    if (e.body.count > 0)
        out.from = e.at(e.at(e.body)[0]).from;
    out.inlined_through = {};
    return out;
}

local_id sgl::check::impl::stage_input_of(flat_builder& out, stage_input input, cc::string_view desired, type_id type)
{
    for (auto const& s : out.e.stage_inputs)
        if (s.input == input)
            return s.local;
    auto const local = out.add_local(local_kind::parameter, desired, type);
    out.e.stage_inputs.push_back({.input = input, .local = local});
    return local;
}

flat_expr_id sgl::check::impl::linear_workgroup(words& w,
                                                local_id group,
                                                cc::fixed_array<i32, 3> const& workgroup,
                                                stmt_list& into,
                                                flat_expr_id* count)
{
    auto& out = w.out;
    auto const grid = listed(out, "sg_dispatch_grid");
    auto const lane = [&](flat_expr_id v, i32 axis) { return out.member(v, axis); };
    auto const id = [&](i32 axis) { return w.unary("convert_int_to_uint", lane(out.local(group), axis)); };
    auto const axis_groups = [&](cc::string_view desired, i32 axis)
    {
        return w.bound(desired,
                       w.binary("divide_uint", lane(out.binding_member(grid, 0), axis), w.word(u32(workgroup[axis]))),
                       into);
    };
    auto const nx = axis_groups("polyfill_nx", 0);
    auto const ny = axis_groups("polyfill_ny", 1);
    if (count != nullptr)
        *count = w.binary("multiply_uint", nx, w.binary("multiply_uint", ny, axis_groups("polyfill_nz", 2)));
    return w.binary("add_uint", id(0),
                    w.binary("multiply_uint", nx, w.binary("add_uint", id(1), w.binary("multiply_uint", ny, id(2)))));
}
