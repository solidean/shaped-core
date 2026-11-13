#include <clean-core/common/assertf.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/legalize/impl/polyfill.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

// CHK-527: the tessellation stages on a target without them are compute passes and a draw.
// The control stage's compute form runs the vertex stage on each control point and the control stage on the patch.
// The prelude's passes count, scan and tessellate each patch; the evaluation stage's compute form runs it at each point.
// The draw reads the points back by the tessellator's indices.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
[[nodiscard]] bool is_stage_input(flat_entry_point const& e, local_id local)
{
    for (auto const& input : e.stage_inputs)
        if (input.local == local)
            return true;
    return false;
}

/// The `n`-th parameter of `e` that is no stage input: the patch first, then an evaluation stage's factors.
[[nodiscard]] local_id value_parameter(flat_entry_point const& e, i32 n)
{
    for (auto i = isize(0); i < e.locals.size(); ++i)
        if (e.locals[i].kind == local_kind::parameter && !is_stage_input(e, local_id(i)) && n-- == 0)
            return local_id(i);
    return local_id::none;
}

/// Where one factor of a factors struct goes in a patch's record: its member, its element (-1 for a scalar), its word.
struct factor_word
{
    i32 member = 0;
    i32 element = -1;
    i32 word = 0;
};

[[nodiscard]] cc::vector<factor_word> factor_words_of(checked_module const& m, type_id factors, i32 domain)
{
    auto result = cc::vector<factor_word>();
    auto const members = m.at(m.at(factors).members);
    for (auto i = isize(0); i < members.size(); ++i)
    {
        auto const& t = m.at(members[i].type);
        if (members[i].factor == tessellation_factor::edge)
            for (auto k = 0; k < t.count; ++k)
                result.push_back({.member = i32(i), .element = k, .word = k});
        else if (members[i].factor == tessellation_factor::inside && domain == 3)
            result.push_back({.member = i32(i), .element = -1, .word = 3});
        else if (members[i].factor == tessellation_factor::inside)
            for (auto k = 0; k < t.count; ++k)
                result.push_back({.member = i32(i), .element = k, .word = 4 + k});
    }
    return result;
}

/// The header of a compute form of a stage that ran inside a polyfilled tessellation, dispatched in workgroups of
/// `k_tessellation_polyfill_workgroup`.
void make_compute_header(flat_entry_point& e, local_id thread, bool is_by_threads)
{
    e.entry_stage = stage::compute;
    e.input = type_id::none;
    e.result = type_id::none;
    e.stage_inputs = {{.input = stage_input::thread_id, .local = thread}};
    e.workgroup = {k_tessellation_polyfill_workgroup, 1, 1};
    e.is_auto_workgroup = is_by_threads;
    e.legalized = nullptr;
    e.features.remove(feature::tessellation_shader);
}
} // namespace

cc::result<tessellation_polyfill_layout, cc::string> sgl::check::tessellation_polyfill_layout_of(checked_module const& m,
                                                                                                 flat_entry_point const& tc,
                                                                                                 flat_entry_point const* te)
{
    CC_ASSERT(tc.entry_stage == stage::tessellation_control, "a layout is made from a tessellation control stage");
    auto const patch = value_parameter(tc, 0);
    if (!is_valid(patch))
        return cc::error(cc::format("{} takes no patch", tc.name));
    auto const& patch_type = m.at(tc.at(patch).type);
    auto const control = leaves_of(m, patch_type.element);
    if (control.has_error())
        return cc::error(cc::format("{}'s control point holds a '{}', which no word holds", tc.name, control.error()));
    auto const factors = leaves_of(m, tc.result);
    if (factors.has_error())
        return cc::error(cc::format("{}'s factors hold a '{}', which no word holds", tc.name, factors.error()));
    auto domain = 0;
    for (auto const& member : m.at(m.at(tc.result).members))
        if (member.factor == tessellation_factor::edge)
            domain = m.at(member.type).count;
    auto const& info = m.functions[m.at(tc.function).info];
    // EMIT-134: HLSL names a control stage's winding in its domain's mirrored orientation, and the tessellator D3D's way
    auto layout = tessellation_polyfill_layout{
        .domain = domain,
        .control_points = patch_type.count,
        .control_words = i32(control.value().size()),
        .factor_words = i32(factors.value().size()),
        .partitioning = i32(info.partitioning),
        .output = domain == 2       ? 2
                : info.is_clockwise ? 1
                                    : 0,
        .max_factor = info.max_factor,
    };
    if (te != nullptr)
    {
        auto const vertex = leaves_of(m, te->result);
        if (vertex.has_error())
            return cc::error(cc::format("{}'s result holds a '{}', which no word holds", te->name, vertex.error()));
        layout.vertex_words = i32(vertex.value().size());
    }
    return layout;
}

cc::result<flat_entry_point, cc::string> sgl::check::tessellation_as_compute(checked_module const& m,
                                                                             flat_entry_point const& v,
                                                                             flat_entry_point const& tc)
{
    CC_ASSERT(v.entry_stage == stage::vertex, "the stage before a polyfilled control stage is a vertex stage");
    auto const layout_or = tessellation_polyfill_layout_of(m, tc, nullptr);
    if (layout_or.has_error())
        return cc::error(layout_or.error());
    auto const layout = layout_or.value();
    auto const patch = value_parameter(tc, 0);
    auto const patch_type = tc.at(patch).type;
    auto const control_leaves = leaves_of(m, m.at(patch_type).element).value();
    auto const factor_leaves = leaves_of(m, tc.result).value();
    auto const factor_words = factor_words_of(m, tc.result, layout.domain);

    auto vertex_input = cc::optional<vertex_layout>();
    if (is_valid(v.input))
    {
        auto read = vertex_layout_of(m, v.input);
        if (read.has_error())
            return cc::error(read.error());
        vertex_input = cc::move(read.value());
    }
    for (auto const& input : v.stage_inputs)
        if (input.input == stage_input::view_index)
            return cc::error(cc::format("{} reads @view_index, which the polyfill does not carry yet", v.name));

    auto out = flat_builder::extend(m, tc);
    if (tc.body.count > 0)
        out.from = tc.at(tc.at(tc.body)[0]).from;
    out.inlined_through = {};
    auto const from = out.from;
    auto w = words{.out = out, .binding = listed(out, "sg_tessellation_controls")};
    auto const records = listed(out, "sg_tessellation_patches");
    auto const draw = listed(out, "sg_geometry_draw");
    auto const draw_value = [&](i32 row, i32 lane)
    { return out.member(out.buffer_element(out.binding_member(draw, 0), w.word(u32(row))), lane); };

    // One thread a patch of one instance, `t = instance * patches + p`, whose words start at `t * patch_words`.
    auto body = stmt_list();
    auto const thread = out.add_local(local_kind::parameter, "thread_id", out.type_named("int3"));
    auto const t = w.bound("tessellation_thread", w.unary("convert_int_to_uint", out.member(out.local(thread), 0)), body);
    auto const patches = w.bound("tessellation_patches", draw_value(0, 1), body);
    auto const instance = w.bound("tessellation_instance", w.binary("divide_uint", t, patches), body);
    auto const p = w.bound("tessellation_patch",
                           w.binary("subtract_uint", t, w.binary("multiply_uint", instance, patches)), body);
    auto const base = w.bound("tessellation_base", w.binary("multiply_uint", t, w.word(u32(layout.patch_words()))), body);

    // each control point: the vertex stage on its vertex, kept for the evaluation stage
    auto points = cc::vector<flat_expr_id>();
    for (auto c = 0; c < layout.control_points; ++c)
    {
        out.from = from;
        out.inlined_through = {};
        auto const k = w.bound(
            "tessellation_control",
            w.binary("add_uint", w.binary("multiply_uint", p, w.word(u32(layout.control_points))), w.word(u32(c))), body);
        auto result = run_vertex_stage(out, w, v, vertex_input, k, instance, body);
        if (result.has_error())
            return cc::error(result.error());
        for (auto i = isize(0); i < control_leaves.size(); ++i)
            body.push_back(out.assign(w.at(base, u32(c * layout.control_words + i)),
                                      w.encode(w.leaf_of(result.value(), control_leaves[i]), control_leaves[i].kind)));
        points.push_back(result.value());
    }

    // the control stage on the patch, its returns the factors, kept whole and handed to the tessellator
    out.from = from;
    out.inlined_through = {};
    out.e.locals[index_of(patch)].kind = local_kind::let;
    body.push_back(out.let(patch, out.construct(patch_type, points)));
    for (auto const& si : tc.stage_inputs)
    {
        out.e.locals[index_of(si.local)].kind = local_kind::let;
        if (si.input != stage_input::primitive_id)
            return cc::error(cc::format("{} reads a stage input the polyfill does not carry yet", tc.name));
        body.push_back(out.let(si.local, w.unary("convert_uint_to_int", p)));
    }
    auto const done = out.add_label("tessellation_control_stage");
    auto const record = w.bound("tessellation_record", w.binary("multiply_uint", t, w.word(8)), body);
    auto const replace = [&](flat_stmt_id sid, stmt_list& into) -> bool
    {
        auto const* const r = out.e.at(sid).node.try_as<flat_return>();
        if (r == nullptr)
            return false;
        out.from = out.e.at(sid).from;
        out.inlined_through = out.e.at(sid).inlined_through;
        auto const factors = w.bound("tessellation_factors", r->value, into);
        auto const first = u32(layout.control_points * layout.control_words);
        for (auto i = isize(0); i < factor_leaves.size(); ++i)
            into.push_back(out.assign(w.at(base, first + u32(i)),
                                      w.encode(w.leaf_of(factors, factor_leaves[i]), factor_leaves[i].kind)));
        for (auto word = 0u; word < 6; ++word)
            into.push_back(out.assign(w.at_of(records, w.binary("add_uint", record, w.word(word))), w.word(0)));
        for (auto const& f : factor_words)
        {
            auto value = out.member(factors, f.member);
            if (f.element >= 0)
                value = out.add_expr(out.m.at(out.e.at(value).type).element,
                                     flat_element{.object = value, .index = out.int_literal(f.element)});
            into.push_back(out.assign(w.at_of(records, w.binary("add_uint", record, w.word(u32(f.word)))),
                                      w.unary("reinterpret_as_uint", value)));
        }
        into.push_back(out.leave(done));
        return true;
    };
    auto const rest = rewritten(out, tc.body, replace);
    auto stage = stmt_list();
    for (auto const s : out.e.at(rest))
        stage.push_back(s);
    out.from = from;
    out.inlined_through = {};
    body.push_back(out.block(done, stage));

    auto& e = out.e;
    make_compute_header(e, thread, true);
    merge_stage_into(e, v);
    e.features.remove(feature::tessellation_shader);
    return compacted(m, e, body);
}

cc::result<flat_entry_point, cc::string> sgl::check::tessellation_evaluation_as_compute(checked_module const& m,
                                                                                        flat_entry_point const& tc,
                                                                                        flat_entry_point const& te)
{
    auto const layout_or = tessellation_polyfill_layout_of(m, tc, &te);
    if (layout_or.has_error())
        return cc::error(layout_or.error());
    auto const layout = layout_or.value();
    auto const patch = value_parameter(te, 0);
    auto const factors = value_parameter(te, 1);
    if (!is_valid(patch) || !is_valid(factors))
        return cc::error(cc::format("{} takes no patch and factors", te.name));
    auto const vertex_leaves = leaves_of(m, te.result).value();

    auto out = flat_builder::extend(m, te);
    if (te.body.count > 0)
        out.from = te.at(te.at(te.body)[0]).from;
    out.inlined_through = {};
    auto const from = out.from;
    auto w = words{.out = out, .binding = listed(out, "sg_mesh_output")};
    auto const controls = listed(out, "sg_tessellation_controls");
    auto const points = listed(out, "sg_tessellation_points");
    auto const args = listed(out, "sg_tessellation_args");
    auto const draw = listed(out, "sg_tessellation_draw");

    // One thread a point, of as many as the scan counted.
    auto body = stmt_list();
    auto const thread = out.add_local(local_kind::parameter, "thread_id", out.type_named("int3"));
    auto const i = w.bound("tessellation_point", w.unary("convert_int_to_uint", out.member(out.local(thread), 0)), body);
    auto inside = stmt_list();
    auto const at = w.bound("tessellation_point_at", w.binary("multiply_uint", i, w.word(3)), inside);
    auto const u = w.bound("tessellation_u", w.at_of(points, at), inside);
    auto const v = w.bound("tessellation_v", w.at_of(points, w.binary("add_uint", at, w.word(1))), inside);
    auto const t = w.bound("tessellation_patch", w.at_of(points, w.binary("add_uint", at, w.word(2))), inside);
    auto const base
        = w.bound("tessellation_base", w.binary("multiply_uint", t, w.word(u32(layout.patch_words()))), inside);

    // the patch and its factors, as the control stage's compute form wrote them
    auto read = words{.out = out, .binding = controls};
    auto next = isize(0);
    out.e.locals[index_of(patch)].kind = local_kind::let;
    inside.push_back(out.let(patch, read.read_value(te.at(patch).type, base, next)));
    out.e.locals[index_of(factors)].kind = local_kind::let;
    inside.push_back(out.let(factors, read.read_value(te.at(factors).type, base, next)));

    // a location in 16.16 is exact as a float, and so is a triangle's third weight in it
    auto const fixed = [&](flat_expr_id x)
    { return w.binary("multiply", w.unary("convert_uint_to_float", x), out.literal(1.0 / 65536.0)); };
    for (auto const& si : te.stage_inputs)
    {
        out.e.locals[index_of(si.local)].kind = local_kind::let;
        auto value = flat_expr_id::none;
        if (si.input == stage_input::domain_location && layout.domain == 3)
        {
            auto const third = w.binary("subtract_uint", w.binary("subtract_uint", w.word(65536), u), v);
            flat_expr_id const lanes[] = {fixed(u), fixed(v), fixed(third)};
            value = out.construct(te.at(si.local).type, lanes);
        }
        else if (si.input == stage_input::domain_location)
        {
            flat_expr_id const lanes[] = {fixed(u), fixed(v)};
            value = out.construct(te.at(si.local).type, lanes);
        }
        else if (si.input == stage_input::primitive_id)
        {
            auto const per_instance = out.member(out.buffer_element(out.binding_member(draw, 0), w.word(0)), 0);
            value = w.unary("convert_uint_to_int", w.binary("remainder_uint", t, per_instance));
        }
        else
            return cc::error(cc::format("{} reads a stage input the polyfill does not carry yet", te.name));
        inside.push_back(out.let(si.local, value));
    }

    // the evaluation stage, its returns the point's words
    auto const done = out.add_label("tessellation_evaluation_stage");
    auto const first = w.bound("mesh_at", w.binary("multiply_uint", i, w.word(u32(layout.vertex_words))), inside);
    auto const replace = [&](flat_stmt_id sid, stmt_list& into) -> bool
    {
        auto const* const r = out.e.at(sid).node.try_as<flat_return>();
        if (r == nullptr)
            return false;
        out.from = out.e.at(sid).from;
        out.inlined_through = out.e.at(sid).inlined_through;
        auto const value = w.bound("tessellation_vertex", r->value, into);
        for (auto k = isize(0); k < vertex_leaves.size(); ++k)
            into.push_back(
                out.assign(w.at(first, u32(k)), w.encode(w.leaf_of(value, vertex_leaves[k]), vertex_leaves[k].kind)));
        into.push_back(out.leave(done));
        return true;
    };
    auto const rest = rewritten(out, te.body, replace);
    auto stage = stmt_list();
    for (auto const s : out.e.at(rest))
        stage.push_back(s);
    out.from = from;
    out.inlined_through = {};
    inside.push_back(out.block(done, stage));
    // the dispatch is the scan's workgroup count, so the threads past its last point return
    auto const total = w.at_of(args, w.word(8));
    body.push_back(out.if_(w.binary("less_uint", i, total), inside));

    auto& e = out.e;
    make_compute_header(e, thread, false);
    return compacted(m, e, body);
}

flat_entry_point sgl::check::tessellation_as_vertex_fetch(checked_module const& m, flat_entry_point const& te)
{
    auto const leaves = leaves_of(m, te.result).value();
    auto out = flat_builder::extend(m, te);
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
    out.e.workgroup = {1, 1, 1};
    out.e.is_auto_workgroup = false;
    out.e.written_words = 0;
    out.e.zeroes_workgroup = false;
    out.e.unshadowed_bytes = 0;
    out.e.workgroup_index = local_id::none;
    out.e.entry_stage = stage::vertex;
    out.e.features = {};
    for (auto const& leaf : leaves)
        if (leaf.kind == leaf_kind::half_)
            out.e.features.set(feature::shader_f16);
    if (te.body.count > 0)
        out.from = te.at(te.at(te.body)[0]).from;
    out.inlined_through = {};

    auto w = words{.out = out, .binding = listed(out, "sg_mesh_input")};
    auto const vertex_index = out.add_local(local_kind::parameter, "vertex_index", out.type_named(builtins::k_int));
    out.e.stage_inputs.push_back({.input = stage_input::vertex_index, .local = vertex_index});
    auto body = stmt_list();
    auto const first = w.bound(
        "mesh_at",
        w.binary("multiply_uint", w.unary("convert_int_to_uint", out.local(vertex_index)), w.word(u32(leaves.size()))),
        body);
    auto next = isize(0);
    auto const result = out.add_local(local_kind::temporary, "mesh_vertex", te.result);
    body.push_back(out.var(result, w.read_value(te.result, first, next)));
    body.push_back(out.return_(out.local(result)));
    return compacted(m, out.e, body);
}

flat_entry_point sgl::check::with_capped_factors(checked_module const& m, flat_entry_point const& tc)
{
    CC_ASSERT(tc.entry_stage == stage::tessellation_control, "only a control stage returns factors");
    auto const cap = m.functions[m.at(tc.function).info].max_factor;
    if (cap >= k_max_tessellation_factor)
        return tc;
    auto out = flat_builder::extend(m, tc);
    auto const& members = m.at(m.at(tc.result).members);
    // factor `k` of member `i`, -1 for a scalar, of `result`: built anew for each use, since an expression has one parent
    auto const factor = [&](local_id result, i32 i, i32 k)
    {
        auto const whole = out.member(out.local(result), i);
        if (k < 0)
            return whole;
        return out.add_expr(m.at(members[i].type).element, flat_element{.object = whole, .index = out.int_literal(k)});
    };
    auto const cap_into = [&](local_id result, i32 i, i32 k, stmt_list& into)
    {
        // `>` is false for a NaN, which reaches the tessellator as it is and culls the patch
        flat_expr_id const compared[] = {factor(result, i, k), out.literal(f64(cap))};
        flat_stmt_id const lowered[] = {out.assign(factor(result, i, k), out.literal(f64(cap)))};
        into.push_back(out.if_(out.call("greater", compared), lowered));
    };
    auto const replace = [&](flat_stmt_id id, stmt_list& into)
    {
        auto const* const r = out.e.at(id).node.try_as<flat_return>();
        if (r == nullptr || !is_valid(r->value))
            return false;
        auto const value = r->value;
        // the replaced return stays in the tree's storage, and must not hold the value as a second parent
        out.e.stmts[index_of(id)].node = flat_return{};
        auto const result = out.add_local(local_kind::temporary, "factors", tc.result);
        out.e.locals[index_of(result)].is_mut = true;
        into.push_back(out.var(result, value));
        for (auto i = 0; i < i32(members.size()); ++i)
        {
            if (members[i].factor == tessellation_factor::none)
                continue;
            auto const& t = m.at(members[i].type);
            if (t.kind != type_kind::array)
                cap_into(result, i, -1, into);
            else
                for (auto k = 0; k < t.count; ++k)
                    cap_into(result, i, k, into);
        }
        into.push_back(out.return_(out.local(result)));
        return true;
    };
    out.e.body = rewritten(out, out.e.body, replace);
    return cc::move(out.e);
}
