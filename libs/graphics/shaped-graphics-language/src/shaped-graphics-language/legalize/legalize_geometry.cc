#include <clean-core/common/assertf.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/legalize/impl/polyfill.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

// CHK-525: a geometry stage on a target without one is a compute pass and a draw.
// One thread a primitive assembles it from the vertex buffers, runs the vertex stage on each corner and the geometry
// stage on the primitive, and writes what the stage appends into the primitive's fixed slots of `sg_mesh_output`.
// The mesh polyfill's vertex form draws them back, so the slots keep the native order: primitive by primitive, then
// in the order each appended.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// The stream parameter of a geometry entry point `e`: `point_stream[T]`, `line_stream[T]` or `triangle_stream[T]`.
[[nodiscard]] local_id stream_of(checked_module const& m, flat_entry_point const& e)
{
    for (auto i = isize(0); i < e.locals.size(); ++i)
    {
        auto const& local = e.locals[i];
        auto const& t = m.at(local.type);
        if (local.kind == local_kind::parameter && t.kind == type_kind::stream && t.count > 0)
            return local_id(i);
    }
    return local_id::none;
}

[[nodiscard]] bool is_stage_input(flat_entry_point const& e, local_id local)
{
    for (auto const& input : e.stage_inputs)
        if (input.local == local)
            return true;
    return false;
}

/// The primitive parameter of a geometry entry point `e`, an array of the vertex stage's result.
[[nodiscard]] local_id primitive_of(checked_module const& m, flat_entry_point const& e)
{
    for (auto i = isize(0); i < e.locals.size(); ++i)
    {
        auto const& local = e.locals[i];
        if (local.kind == local_kind::parameter && m.at(local.type).kind == type_kind::array
            && !is_stage_input(e, local_id(i)))
            return local_id(i);
    }
    return local_id::none;
}

/// What a call of a stream's method writes: 0 for `emit`, 1 for `end_strip`; -1 for every other expression.
[[nodiscard]] i32 stream_write_of(checked_module const& m, flat_expr const& x)
{
    auto const* const call = x.node.try_as<flat_call>();
    auto const* const record = call == nullptr ? nullptr : m.builtin_function(call->intrinsic);
    if (record == nullptr)
        return -1;
    if (record->name == "emit")
        return 0;
    if (record->name == "end_strip")
        return 1;
    return -1;
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

[[nodiscard]] cc::result<vertex_layout, cc::string> vertex_layout_of(checked_module const& m, type_id input)
{
    auto layout = vertex_layout();
    auto streams = cc::vector<cc::string>();
    for (auto const& member : m.at(m.at(input).members))
    {
        auto const name = !member.stream.empty() ? member.stream
                                                 : cc::string(member.is_per_instance ? "per_instance" : "per_vertex");
        auto slot = -1;
        for (auto s = isize(0); s < streams.size(); ++s)
            if (streams[s] == name)
                slot = i32(s);
        if (slot < 0)
        {
            if (streams.size() == k_geometry_polyfill_streams)
                return cc::error(cc::format("{} reads more than {} vertex buffers, which the polyfill does not carry "
                                            "yet",
                                            m.name_of(input), k_geometry_polyfill_streams));
            slot = i32(streams.size());
            streams.push_back(name);
            layout.strides.push_back(0);
        }
        auto words = 0;
        auto const& t = m.at(member.type);
        if (!member.vertex_format.empty())
            words = t.kind == type_kind::array ? t.count : 1;
        else
        {
            auto const leaves = leaves_of(m, member.type);
            if (leaves.has_error())
                return cc::error(cc::format("{}'s member '{}' holds a '{}', which the polyfill does not read yet",
                                            m.name_of(input), member.name, leaves.error()));
            for (auto const& l : leaves.value())
                if (l.kind == leaf_kind::bool_ || l.kind == leaf_kind::half_)
                    return cc::error(cc::format("{}'s member '{}' holds a 16-bit or bool value, which no vertex format "
                                                "is",
                                                m.name_of(input), member.name));
            words = i32(leaves.value().size());
        }
        layout.members.push_back({.stream = slot,
                                  .is_per_instance = member.is_per_instance,
                                  .offset = layout.strides[slot],
                                  .words = words,
                                  .format = member.vertex_format});
        layout.strides[slot] += words;
    }
    return layout;
}

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
} // namespace

cc::result<mesh_polyfill_layout, cc::string> sgl::check::geometry_polyfill_layout_of(checked_module const& m,
                                                                                     flat_entry_point const& g)
{
    CC_ASSERT(g.entry_stage == stage::geometry, "only a geometry entry point has a geometry polyfill layout");
    auto const stream = stream_of(m, g);
    if (!is_valid(stream))
        return cc::error(cc::format("{} takes no stream", g.name));
    auto const& t = m.at(g.at(stream).type);
    if (t.count == 1)
        return cc::error(cc::format("{} appends points, which the polyfill does not draw yet", g.name));
    auto const leaves = leaves_of(m, t.element);
    if (leaves.has_error())
        return cc::error(cc::format("{}'s vertex holds a '{}', which no word holds", g.name, leaves.error()));
    auto const& info = m.functions[m.at(g.function).info];
    auto layout = mesh_polyfill_layout{.corners = t.count,
                                       .max_vertices = info.max_vertices,
                                       .max_primitives = cc::max(1, info.max_vertices - (t.count - 1)),
                                       .vertex_words = i32(leaves.value().size())};
    layout.group_words = layout.first_vertex_word() + layout.max_vertices * layout.vertex_words;
    return layout;
}

cc::result<flat_entry_point, cc::string> sgl::check::geometry_as_compute(checked_module const& m,
                                                                         flat_entry_point const& v,
                                                                         flat_entry_point const& g)
{
    CC_ASSERT(v.entry_stage == stage::vertex, "the stage before a polyfilled geometry stage is a vertex stage");
    auto const layout_or = geometry_polyfill_layout_of(m, g);
    if (layout_or.has_error())
        return cc::error(layout_or.error());
    auto const layout = layout_or.value();
    auto const primitive = primitive_of(m, g);
    auto const stream = stream_of(m, g);
    if (!is_valid(primitive))
        return cc::error(cc::format("{} takes no primitive", g.name));
    auto const primitive_type = g.at(primitive).type;
    auto const corners = m.at(primitive_type).count;
    auto const corner_type = m.at(primitive_type).element;
    auto const leaves = leaves_of(m, m.at(g.at(stream).type).element).value();

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

    auto out = flat_builder::extend(m, g);
    if (g.body.count > 0)
        out.from = g.at(g.at(g.body)[0]).from;
    out.inlined_through = {};
    auto const from = out.from;
    auto w = words{.out = out, .binding = listed(out, "sg_mesh_output")};
    auto const draw = listed(out, "sg_geometry_draw");
    auto const draw_value = [&](i32 row, i32 lane)
    { return out.member(out.buffer_element(out.binding_member(draw, 0), w.word(u32(row))), lane); };
    auto const as_int = [&](flat_expr_id x) { return w.unary("convert_uint_to_int", x); };
    auto const var_of = [&](cc::string_view name, type_id type, flat_expr_id value, stmt_list& into)
    {
        auto const local = add_temporary_var(out, name, type);
        into.push_back(out.var(local, value));
        return local;
    };

    // One thread a primitive of one instance: `t = instance * primitives + p`, whose slots start at `t * group_words`.
    auto body = stmt_list();
    auto const thread = out.add_local(local_kind::parameter, "thread_id", out.type_named("int3"));
    auto const t = w.bound("geometry_thread", w.unary("convert_int_to_uint", out.member(out.local(thread), 0)), body);
    auto const primitives = w.bound("geometry_primitives", draw_value(0, 1), body);
    auto const instance = w.bound("geometry_instance", w.binary("divide_uint", t, primitives), body);
    auto const p = w.bound("geometry_primitive",
                           w.binary("subtract_uint", t, w.binary("multiply_uint", instance, primitives)), body);
    auto const base = w.bound("mesh_base", w.binary("multiply_uint", t, w.word(u32(layout.group_words))), body);
    // the counts start at nothing, so a geometry stage that returns before it appends leaves its slots empty
    body.push_back(out.assign(w.at(base, 0), w.word(0)));
    body.push_back(out.assign(w.at(base, 1), w.word(0)));

    // Each corner: its vertex from the index buffer or the draw's range, then the vertex stage on it.
    auto results = cc::vector<flat_expr_id>();
    for (auto c = 0; c < corners; ++c)
    {
        out.from = from;
        out.inlined_through = {};
        auto const k
            = w.bound("geometry_corner",
                      w.binary("add_uint", w.binary("multiply_uint", p, w.word(u32(corners))), w.word(u32(c))), body);
        auto const index_kind = w.bound("geometry_index_kind", draw_value(0, 2), body);
        auto const number = var_of("geometry_vertex", w.uint_type(), w.binary("add_uint", draw_value(0, 0), k), body);
        auto const counted = var_of("geometry_vertex_index", w.uint_type(), k, body);
        {
            // a 16-bit index is half of a word, the low half first
            auto const indices = listed(out, "sg_geometry_indices");
            auto const position = w.binary("add_uint", draw_value(0, 0), w.binary("add_uint", k, draw_value(1, 1)));
            auto indexed = stmt_list();
            auto const at = w.bound("geometry_index_at", position, indexed);
            auto const index = var_of("geometry_index", w.uint_type(), w.at_of(indices, at), indexed);
            auto const half
                = w.binary("bit_and_uint",
                           w.binary("shift_right_uint", w.at_of(indices, w.binary("shift_right_uint", at, w.word(1))),
                                    w.binary("multiply_uint", w.binary("bit_and_uint", at, w.word(1)), w.word(16))),
                           w.word(0xffff));
            flat_stmt_id const as_half[] = {out.assign(out.local(index), half)};
            indexed.push_back(out.if_(w.binary("equal_uint", index_kind, w.word(1)), as_half));
            indexed.push_back(out.assign(out.local(number), w.binary("add_uint", out.local(index), draw_value(0, 3))));
            indexed.push_back(out.assign(out.local(counted), out.local(index)));
            body.push_back(out.if_(w.binary("not_equal_uint", index_kind, w.word(0)), indexed));
        }

        // the stage struct, each member from its stream's words at its element there
        auto input = flat_expr_id::none;
        if (vertex_input.has_value())
        {
            auto const& vl = vertex_input.value();
            auto const per_instance
                = w.bound("geometry_instance_number", w.binary("add_uint", draw_value(1, 0), instance), body);
            auto const members = m.at(m.at(v.input).members);
            auto parts = cc::vector<flat_expr_id>();
            for (auto i = isize(0); i < members.size(); ++i)
            {
                auto const& vm = vl.members[i];
                auto const stream_words = listed(out, cc::format("sg_geometry_vertices{}", vm.stream));
                auto const element = vm.is_per_instance ? per_instance : out.local(number);
                auto const first = w.bound(
                    "geometry_at",
                    w.binary("add_uint", draw_value(2, vm.stream),
                             w.binary("add_uint", w.binary("multiply_uint", element, w.word(u32(vl.strides[vm.stream]))),
                                      w.word(u32(vm.offset)))),
                    body);
                auto const read = [&](u32 offset)
                { return w.at_of(stream_words, offset == 0 ? first : w.binary("add_uint", first, w.word(offset))); };
                auto const packed = [&](flat_expr_id word, type_id type)
                {
                    if (vm.format == "rgba8_unorm")
                        return w.unary("unpack_unorm4x8", word);
                    auto lanes = cc::vector<flat_expr_id>();
                    for (auto lane = 0u; lane < 4; ++lane)
                        lanes.push_back(w.binary("bit_and_uint", w.binary("shift_right_uint", word, w.word(8 * lane)),
                                                 w.word(0xff)));
                    return out.construct(type, lanes);
                };
                auto const& mt = m.at(members[i].type);
                if (!vm.format.empty() && mt.kind == type_kind::array)
                {
                    auto elements = cc::vector<flat_expr_id>();
                    for (auto e = 0; e < mt.count; ++e)
                        elements.push_back(packed(read(u32(e)), mt.element));
                    parts.push_back(out.construct(members[i].type, elements));
                }
                else if (!vm.format.empty())
                    parts.push_back(packed(read(0), members[i].type));
                else
                {
                    auto next = isize(0);
                    auto member_words = words{.out = out, .binding = stream_words};
                    parts.push_back(member_words.read_value(members[i].type, first, next));
                }
            }
            input = w.bound("geometry_input", out.construct(v.input, parts), body);
        }

        // the vertex stage, its returns the corner's value
        auto const result = add_temporary_var(out, "geometry_corner_value", corner_type);
        body.push_back(out.var(result));
        auto const done = out.add_label("geometry_vertex_stage");
        auto const on_return = [&](flat_expr_id value, stmt_list& into)
        {
            into.push_back(out.assign(out.local(result), value));
            into.push_back(out.leave(done));
        };
        auto g1 = grafter{.in = v, .out = out, .on_return = on_return};
        g1.prepare();
        out.from = from;
        out.inlined_through = {};
        auto stage = stmt_list();
        if (is_valid(v.input))
        {
            auto const local = g1.local(local_id(0));
            out.e.locals[index_of(local)].kind = local_kind::let;
            stage.push_back(out.let(local, input));
        }
        for (auto const& si : v.stage_inputs)
        {
            auto const local = g1.local(si.local);
            out.e.locals[index_of(local)].kind = local_kind::let;
            auto value = flat_expr_id::none;
            if (si.input == stage_input::vertex_index)
                value = as_int(out.local(counted));
            else if (si.input == stage_input::instance_index)
                value = as_int(instance);
            else if (si.input == stage_input::first_instance)
                value = as_int(draw_value(1, 0));
            if (!is_valid(value))
                return cc::error(cc::format("{} reads a stage input the polyfill does not carry yet", v.name));
            stage.push_back(out.let(local, value));
        }
        for (auto const s : g1.statements(v.body))
            stage.push_back(s);
        body.push_back(out.block(done, stage));
        results.push_back(out.local(result));
    }

    // the geometry stage's parameters, from what the corners computed
    out.from = from;
    out.inlined_through = {};
    out.e.locals[index_of(primitive)].kind = local_kind::let;
    body.push_back(out.let(primitive, out.construct(primitive_type, results)));
    for (auto const& si : g.stage_inputs)
    {
        out.e.locals[index_of(si.local)].kind = local_kind::let;
        if (si.input != stage_input::primitive_id)
            return cc::error(cc::format("{} reads a stage input the polyfill does not carry yet", g.name));
        body.push_back(out.let(si.local, as_int(p)));
    }
    out.e.locals[index_of(stream)].kind = local_kind::temporary;

    // What the stage appends: vertices in order, cut into primitives where a strip holds enough of them.
    auto const appended = var_of("geometry_vertices", w.uint_type(), w.word(0), body);
    auto const made = var_of("geometry_primitives_made", w.uint_type(), w.word(0), body);
    auto const strip = var_of("geometry_strip", w.uint_type(), w.word(0), body);
    auto const replace = [&](flat_stmt_id sid, stmt_list& into) -> bool
    {
        auto const* const eval = out.e.at(sid).node.try_as<flat_eval>();
        if (eval == nullptr || !is_valid(eval->value))
            return false;
        auto const write = stream_write_of(m, out.e.at(eval->value));
        if (write < 0)
            return false;
        auto const& s = out.e.at(sid);
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        if (write == 1)
        {
            into.push_back(out.assign(out.local(strip), w.word(0)));
            return true;
        }
        auto const arguments = cc::vector<flat_expr_id>::create_copy_of(
            cc::span<flat_expr_id const>(out.e.at(out.e.at(eval->value).node.as<flat_call>().arguments)));
        auto const value = w.bound("geometry_appended", arguments[1], into);
        auto const n = out.local(appended);
        auto room = stmt_list();
        auto const at = w.bound("mesh_at",
                                w.binary("add_uint", base,
                                         w.binary("add_uint", w.word(u32(layout.first_vertex_word())),
                                                  w.binary("multiply_uint", n, w.word(u32(layout.vertex_words))))),
                                room);
        for (auto i = isize(0); i < leaves.size(); ++i)
            room.push_back(out.assign(w.at(at, u32(i)), w.encode(w.leaf_of(value, leaves[i]), leaves[i].kind)));

        // a strip's primitive `j` is its corners `j` on; an odd triangle swaps its first two, so every one winds alike
        auto cut = stmt_list();
        auto const indices_at
            = w.bound("mesh_indices_at",
                      w.binary("add_uint", base,
                               w.binary("add_uint", w.word(u32(layout.first_index_word())),
                                        w.binary("multiply_uint", out.local(made), w.word(u32(layout.corners))))),
                      cut);
        auto const corner = [&](u32 back) { return w.binary("subtract_uint", out.local(appended), w.word(back)); };
        if (layout.corners == 3)
        {
            auto const odd
                = w.bound("geometry_odd",
                          w.binary("equal_uint", w.binary("bit_and_uint", out.local(strip), w.word(1)), w.word(1)), cut);
            auto const select = [&](flat_expr_id a, flat_expr_id b)
            {
                flat_expr_id const arguments[] = {odd, a, b};
                return out.call("select", arguments);
            };
            auto const first = w.bound("geometry_first", select(corner(1), corner(2)), cut);
            auto const second = w.bound("geometry_second", select(corner(2), corner(1)), cut);
            cut.push_back(out.assign(w.at(indices_at, 0), first));
            cut.push_back(out.assign(w.at(indices_at, 1), second));
            cut.push_back(out.assign(w.at(indices_at, 2), out.local(appended)));
        }
        else
        {
            cut.push_back(out.assign(w.at(indices_at, 0), corner(1)));
            cut.push_back(out.assign(w.at(indices_at, 1), out.local(appended)));
        }
        cut.push_back(out.assign(out.local(made), w.binary("add_uint", out.local(made), w.word(1))));
        cut.push_back(out.assign(w.at(base, 1), out.local(made)));
        room.push_back(out.if_(out.and_(w.binary("greater_equal_uint", out.local(strip), w.word(u32(layout.corners - 1))),
                                        w.binary("less_uint", out.local(made), w.word(u32(layout.max_primitives)))),
                               cut));

        room.push_back(out.assign(out.local(appended), w.binary("add_uint", out.local(appended), w.word(1))));
        room.push_back(out.assign(w.at(base, 0), out.local(appended)));
        room.push_back(out.assign(out.local(strip), w.binary("add_uint", out.local(strip), w.word(1))));
        // what a stage appends past `max_vertices` is dropped, as the native stage drops it
        into.push_back(out.if_(w.binary("less_uint", n, w.word(u32(layout.max_vertices))), room));
        return true;
    };
    auto const rest = rewritten(out, g.body, replace);
    for (auto const s : out.e.at(rest))
        body.push_back(s);

    // the header of a compute entry point dispatched by threads, whose threads past the grid return (CHK-520)
    auto& e = out.e;
    e.entry_stage = stage::compute;
    e.input = type_id::none;
    e.result = type_id::none;
    e.stage_inputs = {{.input = stage_input::thread_id, .local = thread}};
    e.workgroup = {k_geometry_polyfill_workgroup, 1, 1};
    e.is_auto_workgroup = true;
    e.legalized = nullptr;
    // CHK-184: the stages list the same binding at every position they share, so the longer list holds the other
    if (v.bindings.size() > e.bindings.size())
        e.bindings = v.bindings;
    merge_into(e.reserved, cc::span<symbol_id const>(v.reserved));
    merge_into(e.options, cc::span<symbol_id const>(v.options));
    merge_into(e.traced_rays, cc::span<flat_traced_ray const>(v.traced_rays));
    merge_into(e.text_types, cc::span<flat_text_type const>(v.text_types));
    for (auto i = isize(0); i < k_feature_count; ++i)
        if (v.features.has(feature(i)))
            e.features.set(feature(i));
    e.features.remove(feature::geometry_shader);
    if (u8(v.float_guarantee) > u8(e.float_guarantee))
    {
        e.float_guarantee = v.float_guarantee;
        e.float_raised_by = v.float_raised_by;
    }
    return compacted(m, e, body);
}

flat_entry_point sgl::check::geometry_as_vertex_fetch(checked_module const& m, flat_entry_point const& g)
{
    auto const layout = geometry_polyfill_layout_of(m, g);
    CC_ASSERTF(layout.has_value(), "{}", layout.has_error() ? layout.error() : cc::string());
    return slots_as_vertex_fetch(m, g, layout.value(), m.at(g.at(stream_of(m, g)).type).element);
}
