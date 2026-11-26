#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ir/impl/lowering.hh>
#include <shaped-graphics-language/ir/lower.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::ir;
using namespace sgl::ir::impl;

lowering::lowering(emit::impl::plan const& plan, program& out) : _plan(plan), _m(plan.m), _e(plan.e), _out(out)
{
    _guarantee = _e.float_guarantee;
    _out.entry_name = plan.entry_name;
    _out.guarantee = _e.float_guarantee;
    for (auto axis = 0; axis < 3; ++axis)
        _out.workgroup[axis] = _e.workgroup[axis];

    _out.stage = _e.entry_stage;
    _value_types.resize_to_filled(_m.types.size(), type_ref::none);
    _constant_slots.resize_to_filled(256, 0u);
    _locals.resize_to_filled(_e.locals.size(), value_ref::none);
    _is_variable.resize_to_filled(_e.locals.size(), false);
    _variable_of.resize_to_filled(_e.locals.size(), k_no_element);
    _parameter_elements.resize_to_filled(_e.locals.size(), k_no_element);
    _block_globals.resize_to_filled(plan.group_blocks.size(), value_ref::none);
    _resource_globals.resize_to_filled(plan.resources.size(), value_ref::none);
    _workgroup_globals.resize_to_filled(plan.workgroup.size(), value_ref::none);
    _sampler_globals.resize_to_filled(plan.samplers.size(), value_ref::none);

    _out.instructions.reserve(_e.exprs.size() * 2);
    _out.operands.reserve(_e.exprs.size() * 4);

    _void = intern({.kind = type_kind::void_});
    _bool = intern({.kind = type_kind::bool_});
    _int = intern({.kind = type_kind::int_, .width = 32, .is_signed = true});
    _uint = intern({.kind = type_kind::int_, .width = 32});
    _float = intern({.kind = type_kind::float_, .width = 32});
}

void lowering::unsupported(cc::string_view what)
{
    if (_failure.empty())
        _failure = cc::string(what);
}

u32 lowering::add_name(cc::string_view name)
{
    auto const first = u32(_out.names.size());
    _out.names += name;
    return first;
}

block_ref lowering::new_block()
{
    _out.blocks.push_back({});
    return block_ref(_out.blocks.size() - 1);
}

void lowering::open_block(block_ref b)
{
    // blocks are written in the order they open, which is a structured tree's dominance order
    _open = b;
    _is_terminated = false;
    _out.blocks[isize(b)].first = u32(_out.instructions.size());
}

void lowering::set_merge(merge_kind kind, block_ref merge, block_ref continue_to)
{
    auto& b = _out.blocks[isize(_open)];
    b.merges = kind;
    b.merge = merge;
    b.continue_to = continue_to;
}

value_ref lowering::add(op code, type_ref type, cc::span<u32 const> operands, u8 sub, u8 aux)
{
    // what follows a `break` or a `return` in its list never runs
    if (_is_terminated)
        return value_ref::none;
    if (u8(_guarantee) >= u8(float_level::precise))
        if (auto const strict = strict_form(code, type, operands, sub); is_valid(strict))
            return strict;
    auto const index = u32(_out.instructions.size());
    auto const first = u32(_out.operands.size());
    for (auto const operand : operands)
        _out.operands.push_back(operand);
    _out.instructions.push_back({.code = code,
                                 .sub = sub,
                                 .aux = aux,
                                 .guarantee = _guarantee,
                                 .type = type,
                                 .first = first,
                                 .count = u32(operands.size()),
                                 .statement = _statement});
    if (code >= op::branch)
    {
        _out.blocks[isize(_open)].count = index + 1 - _out.blocks[isize(_open)].first;
        _is_terminated = true;
    }
    return value_ref(index);
}

value_ref lowering::strict_form(op code, type_ref type, cc::span<u32 const> operands, u8 sub)
{
    switch (code)
    {
    case op::dot:
    {
        if (_out.at(type).kind != type_kind::float_)
            return value_ref::none;
        auto const a = value_ref(operands[0]);
        return ordered_dot(a, value_ref(operands[1]), type, lanes_of(_out.type_of(a)));
    }
    case op::matrix_times_vector:
    {
        // row r of `m * v` is row r of `m` dotted with `v`; `m` holds its columns, so row r is lane r of each
        auto const m = value_ref(operands[0]);
        auto const v = value_ref(operands[1]);
        auto const matrix = _out.at(_out.type_of(m));
        auto const scalar = scalar_of(type);
        value_ref columns[4];
        value_ref lanes[4];
        for (auto c = u32(0); c < matrix.count; ++c)
        {
            columns[c] = extracted(m, matrix.element, c);
            lanes[c] = extracted(v, scalar, c);
        }
        u32 rows[4];
        for (auto r = u32(0); r < lanes_of(type); ++r)
        {
            auto sum = value_ref::none;
            for (auto c = u32(0); c < matrix.count; ++c)
            {
                auto const product = add(op::fmul, scalar, {extracted(columns[c], scalar, r), lanes[c]});
                sum = c == 0 ? product : add(op::fadd, scalar, {sum, product});
            }
            rows[r] = u32(sum);
        }
        return add(op::construct, type, cc::span<u32 const>(rows, isize(lanes_of(type))));
    }
    case op::matrix_times_matrix:
    {
        // column j of `a * b` is `a` times column j of `b`
        auto const a = value_ref(operands[0]);
        auto const b = value_ref(operands[1]);
        auto const result = _out.at(type);
        auto const right = _out.at(_out.type_of(b));
        u32 columns[4];
        for (auto j = u32(0); j < result.count; ++j)
            columns[j] = u32(add(op::matrix_times_vector, result.element, {a, extracted(b, right.element, j)}));
        return add(op::construct, type, cc::span<u32 const>(columns, isize(result.count)));
    }
    case op::math:
        return strict_math(math(sub), type, operands);
    default:
        return value_ref::none;
    }
}

value_ref lowering::strict_math(math fn, type_ref type, cc::span<u32 const> operands)
{
    auto const scalar = scalar_of(type);
    auto const constant = [&](f64 value, type_ref of) { return splat(float_constant(scalar, value), of); };
    auto const length_of = [&](value_ref v)
    {
        auto const s = scalar_of(_out.type_of(v));
        return add(op::math, s, {ordered_dot(v, v, s, lanes_of(_out.type_of(v)))}, u8(math::sqrt));
    };
    switch (fn)
    {
    case math::smooth_step:
    {
        // t = saturate((x - e0) / (e1 - e0)), then t * (t * (3 - 2t)): DXC's grouping, so every route computes one value.
        // It and the written definition's (t * t) * (3 - 2t) each round three times with no cancellation, since 2t is
        // exact and 3 - 2t is at least 1, so both are within about 3 * 2^-24 of it, and both are exact at 0 and 1.
        // This one is kept: t * t underflows to zero for a t below about 1e-19 where denormals are flushed, and t
        // times the larger 3 - 2t first does not, so it is also the closer of the two where t is tiny.
        auto const low = value_ref(operands[0]);
        auto const high = value_ref(operands[1]);
        auto const x = value_ref(operands[2]);
        auto const raw = add(op::fdiv, type, {add(op::fsub, type, {x, low}), add(op::fsub, type, {high, low})});
        auto const t = add(op::math, type, {raw, constant(0.0, type), constant(1.0, type)}, u8(math::fclamp));
        auto const twice = add(op::fmul, type, {constant(2.0, type), t});
        auto const shaped = add(op::fsub, type, {constant(3.0, type), twice});
        return add(op::fmul, type, {t, add(op::fmul, type, {t, shaped})});
    }
    case math::cross:
    {
        // each lane a_p * b_q - a_q * b_p, as DXC writes `cross`: SPIR-V's own is an extended instruction no
        // `NoContraction` reaches, which a driver may fuse
        auto const a = value_ref(operands[0]);
        auto const b = value_ref(operands[1]);
        auto const minor = [&](u32 p, u32 q)
        {
            return u32(add(op::fsub, scalar,
                           {add(op::fmul, scalar, {extracted(a, scalar, p), extracted(b, scalar, q)}),
                            add(op::fmul, scalar, {extracted(a, scalar, q), extracted(b, scalar, p)})}));
        };
        u32 const lanes[] = {minor(1, 2), minor(2, 0), minor(0, 1)};
        return add(op::construct, type, lanes);
    }
    case math::fmix:
        // a + t * (b - a), DXC's lowering of `lerp`, rather than a * (1 - t) + b * t: one product, and exact at t = 0
        return add(
            op::fadd, type,
            {value_ref(operands[0]),
             add(op::fmul, type,
                 {value_ref(operands[2]), add(op::fsub, type, {value_ref(operands[1]), value_ref(operands[0])})})});
    case math::length:
        // sqrt of the ordered dot of v with itself, as DXC writes `length`
        return length_of(value_ref(operands[0]));
    case math::distance:
        // the length of a - b
        return length_of(
            add(op::fsub, _out.type_of(value_ref(operands[0])), {value_ref(operands[0]), value_ref(operands[1])}));
    case math::normalize:
    {
        // v times 1 / length(v): DXC's `normalize` multiplies by `rsqrt` instead, but `rsqrt` is no operation SPIR-V and
        // DXIL compute alike, and a division of a square root is, on every route; so all three write this one
        auto const v = value_ref(operands[0]);
        auto const inverse = add(op::fdiv, scalar, {float_constant(scalar, 1.0), length_of(v)});
        return add(op::vector_times_scalar, type, {v, inverse});
    }
    case math::reflect:
    {
        // i - n * (2 * dot(n, i)): the scalar factor first, as DXC writes it, then multiplied into n, then subtracted
        auto const i = value_ref(operands[0]);
        auto const n = value_ref(operands[1]);
        auto const k = add(op::fmul, scalar, {float_constant(scalar, 2.0), ordered_dot(n, i, scalar, lanes_of(type))});
        return add(op::fsub, type, {i, add(op::vector_times_scalar, type, {n, k})});
    }
    case math::refract:
    {
        // k = 1 - eta * eta * (1 - d * d) of d = dot(n, i), then eta * i - (eta * d + sqrt(k)) * n where k >= 0 and zero
        // elsewhere, each step as DXC writes `refract`; a NaN k gives zero, as DXC's ordered compare does
        auto const i = value_ref(operands[0]);
        auto const n = value_ref(operands[1]);
        auto const eta = value_ref(operands[2]);
        auto const one = float_constant(scalar, 1.0);
        auto const zero = float_constant(scalar, 0.0);
        auto const d = ordered_dot(n, i, scalar, lanes_of(type));
        auto const sine = add(op::fsub, scalar, {one, add(op::fmul, scalar, {d, d})});
        auto const k = add(op::fsub, scalar, {one, add(op::fmul, scalar, {add(op::fmul, scalar, {eta, eta}), sine})});
        auto const term
            = add(op::fadd, scalar, {add(op::fmul, scalar, {eta, d}), add(op::math, scalar, {k}, u8(math::sqrt))});
        auto const bent
            = add(op::fsub, type,
                  {add(op::vector_times_scalar, type, {i, eta}), add(op::vector_times_scalar, type, {n, term})});
        auto const is_bent = add(op::fge, _bool, {k, zero});
        u32 lanes[4];
        for (auto l = u32(0); l < lanes_of(type); ++l)
            lanes[l] = u32(add(op::select, scalar, {is_bent, extracted(bent, scalar, l), zero}));
        return add(op::construct, type, cc::span<u32 const>(lanes, isize(lanes_of(type))));
    }
    default:
        return value_ref::none;
    }
}

value_ref lowering::ordered_dot(value_ref a, value_ref b, type_ref scalar, u32 lanes)
{
    auto sum = add(op::fmul, scalar, {extracted(a, scalar, 0), extracted(b, scalar, 0)});
    for (auto l = u32(1); l < lanes; ++l)
        sum = add(op::fadd, scalar, {sum, add(op::fmul, scalar, {extracted(a, scalar, l), extracted(b, scalar, l)})});
    return sum;
}

value_ref lowering::add(op code, type_ref type, std::initializer_list<value_ref> operands, u8 sub, u8 aux)
{
    u32 raw[8];
    auto count = isize(0);
    for (auto const v : operands)
        raw[count++] = u32(v);
    return add(code, type, cc::span<u32 const>(raw, count), sub, aux);
}

void lowering::branch(block_ref to)
{
    u32 const operands[] = {u32(to)};
    add(op::branch, type_ref::none, operands);
}

void lowering::branch_if(value_ref condition, block_ref yes, block_ref no)
{
    u32 const operands[] = {u32(condition), u32(yes), u32(no)};
    add(op::branch_conditional, type_ref::none, operands);
}

void lowering::run()
{
    auto const is_raster = _e.entry_stage == stage::vertex || _e.entry_stage == stage::pixel;
    auto const is_primitive = _e.entry_stage == stage::geometry || _e.entry_stage == stage::tessellation_control
                           || _e.entry_stage == stage::tessellation_evaluation || _e.entry_stage == stage::mesh;
    if (_e.entry_stage != stage::compute && _e.entry_stage != stage::task && !is_raster && !is_primitive
        && !is_ray_stage())
    {
        unsupported(cc::format("a {} entry point, whose stage is not lowered yet", stage_name(_e.entry_stage)));
        // what crosses its edge stands on its own, and DXIL's packer reads it ahead of any lowering of the body
        return declare_stage_elements();
    }
    _out.sees_micromaps = _plan.states_micromaps;
    _out.reorders_through_nvapi = _plan.reorders_through_nvapi;
    _out.draw_start_in_root_constants = _plan.draw_start_in_root_constants;
    _out.maximally_reconverges = _e.maximally_reconverges;
    _out.early_depth_test = _e.entry_stage == check::stage::pixel && _e.is_early_depth_test;

    open_block(new_block());
    if (is_raster)
    {
        declare_stage_elements();
        read_stage_edge();
    }
    else if (is_primitive)
    {
        declare_stage_elements();
        describe_primitive_stage();
        read_primitive_edge();
    }
    else if (is_ray_stage())
    {
        read_ray_edge();
        annotate_payloads();
    }
    else
    {
        declare_stage_inputs();
    }
    statements(_e.body);
    if (!_is_terminated)
        add(op::return_, type_ref::none, cc::span<u32 const>());
}

void lowering::declare_stage_inputs()
{
    for (auto i = isize(0); i < _e.stage_inputs.size(); ++i)
    {
        auto const& input = _e.stage_inputs[i];
        if (i < _plan.stage_input_read.size() && !_plan.stage_input_read[i])
            continue;
        switch (input.input)
        {
        case stage_input::thread_id:
        case stage_input::local_thread_id:
        case stage_input::local_thread_index:
        case stage_input::workgroup_id:
        case stage_input::subgroup_size:
        case stage_input::subgroup_invocation_id:
            break;
        default:
            return unsupported("a stage input of no compute entry point");
        }
        // the target hands every one over unsigned, and the program reads it as it declared it
        u32 const operands[] = {element_of_input(i)};
        auto const raw = _out.stage_elements[operands[0]].type;
        auto const loaded = add(op::stage_load, raw, operands);
        auto const declared = value_type(_e.at(input.local).type);
        _locals[index_of(input.local)] = raw == declared ? loaded : add(op::bitcast, declared, {loaded});
        describe(input.local, _locals[index_of(input.local)], false);
    }
}

value_ref lowering::written_shadow()
{
    if (!is_valid(_written))
    {
        auto const words = intern({.kind = type_kind::array, .count = u32(_e.written_words), .element = _uint});
        _written = add_global({.type = pointer_type(words, storage::workgroup)}, "sgl_written");
    }
    return _written;
}

void lowering::statements(ast::range_of<flat_stmt_id> range)
{
    for (auto const id : _e.at(range))
    {
        if (_is_terminated || !_failure.empty())
            return;
        statement(id);
    }
}

void lowering::statement(flat_stmt_id id)
{
    auto const& s = _e.at(id);
    _statement = u32(index_of(id));
    s.node.visit(
        [&](flat_let const& let) { declare(let.local, let.value, _e.at(let.local).is_mut); }, [&](flat_var const& var)
        { declare(var.local, var.value, true); }, [&](flat_assign const& a) { assign(a); }, [&](flat_print const&)
        { unsupported("a print legalize left"); }, [&](flat_eval const& v) { (void)expr(v.value); }, //
        [&](flat_if const& i) { if_(i); }, [&](flat_loop const& l) { loop_(l); },                    //
        [&](flat_while const& w) { while_(w); }, [&](flat_for const& f) { for_(f); },                //
        [&](flat_continue const&) { branch(_continues.back()); }, [&](flat_discard const&)
        { add(op::discard, type_ref::none, cc::span<u32 const>()); }, [&](flat_once const& o) { once_(o); }, //
        [&](flat_break const&) { branch(_breaks.back()); }, [&](flat_switch const& sw) { switch_(sw); },
        [&](flat_return const& r)
        {
            if (is_valid(r.value) && _e.entry_stage == stage::any_hit)
                decide_hit(expr(r.value));
            else if (is_valid(r.value) && _e.entry_stage == stage::intersection)
                report_intersection(expr(r.value), _e.at(r.value).type);
            else if (is_valid(r.value))
                store_stage_result(expr(r.value));
            add(op::return_, type_ref::none, cc::span<u32 const>());
        },
        // none of these is in a core tree
        [&](flat_block const&) {}, [&](flat_leave const&) {}, [&](flat_case const&) {}, [&](flat_check const&) {});
}

void lowering::if_(flat_if const& s)
{
    auto const condition = expr(s.condition);
    auto const has_else = !_e.at(s.else_body).empty();
    auto const then_block = new_block();
    auto const else_block = has_else ? new_block() : block_ref::none;
    auto const merge = new_block();
    set_merge(merge_kind::selection, merge);
    branch_if(condition, then_block, has_else ? else_block : merge);

    open_block(then_block);
    statements(s.then_body);
    branch(merge);
    if (has_else)
    {
        open_block(else_block);
        statements(s.else_body);
        branch(merge);
    }
    open_block(merge);
}

void lowering::loop_(flat_loop const& s)
{
    auto const header = new_block();
    auto const body = new_block();
    auto const next = new_block();
    auto const merge = new_block();
    branch(header);

    open_block(header);
    set_merge(merge_kind::loop, merge, next);
    branch(body);

    _breaks.push_back(merge);
    _continues.push_back(next);
    open_block(body);
    statements(s.body);
    branch(next);
    _breaks.pop_back();
    _continues.pop_back();

    open_block(next);
    branch(header);
    open_block(merge);
}

void lowering::while_(flat_while const& s)
{
    auto const header = new_block();
    auto const test = new_block();
    auto const body = new_block();
    auto const next = new_block();
    auto const merge = new_block();
    branch(header);

    open_block(header);
    set_merge(merge_kind::loop, merge, next);
    branch(test);

    open_block(test);
    branch_if(expr(s.condition), body, merge);

    _breaks.push_back(merge);
    _continues.push_back(next);
    open_block(body);
    statements(s.body);
    branch(next);
    _breaks.pop_back();
    _continues.pop_back();

    open_block(next);
    branch(header);
    open_block(merge);
}

void lowering::for_(flat_for const& s)
{
    // both bounds once, `first` before `end`, ahead of the first iteration
    auto const first = expr(s.first);
    auto const end = expr(s.end);
    auto const counter = add_local(_int, _e.at(s.index).name);
    add(op::store, type_ref::none, {counter, first});
    describe(s.index, counter, true);

    auto const header = new_block();
    auto const test = new_block();
    auto const body = new_block();
    auto const next = new_block();
    auto const merge = new_block();
    branch(header);

    open_block(header);
    set_merge(merge_kind::loop, merge, next);
    branch(test);

    open_block(test);
    auto const at = add(op::load, _int, {counter});
    branch_if(add(op::slt, _bool, {at, end}), body, merge);

    _breaks.push_back(merge);
    _continues.push_back(next);
    open_block(body);
    _locals[index_of(s.index)] = add(op::load, _int, {counter});
    statements(s.body);
    branch(next);
    _breaks.pop_back();
    _continues.pop_back();

    open_block(next);
    auto const before = add(op::load, _int, {counter});
    add(op::store, type_ref::none, {counter, add(op::iadd, _int, {before, int_constant(1)})});
    branch(header);
    open_block(merge);
}

void lowering::once_(flat_once const& s)
{
    auto const header = new_block();
    auto const body = new_block();
    auto const next = new_block();
    auto const merge = new_block();
    branch(header);

    open_block(header);
    set_merge(merge_kind::loop, merge, next);
    branch(body);

    // a `continue` never crosses it in a core tree, so only a `break` reaches it
    _breaks.push_back(merge);
    open_block(body);
    statements(s.body);
    branch(next);
    _breaks.pop_back();

    // SPIR-V's loop needs a back edge, which this one never takes
    open_block(next);
    branch_if(bool_constant(false), header, merge);
    open_block(merge);
}

void lowering::switch_(flat_switch const& s)
{
    auto const selector = expr(s.scrutinee);
    auto const merge = new_block();
    auto const fallback = new_block();
    auto const arms = cc::vector<flat_arm>::create_copy_of(cc::span<flat_arm const>(_e.at(s.arms)));
    auto blocks = cc::vector<block_ref>();
    auto operands = cc::vector<u32>();
    operands.push_back(u32(selector));
    operands.push_back(u32(fallback));
    for (auto const& arm : arms)
    {
        auto const b = new_block();
        blocks.push_back(b);
        for (auto const pattern : _e.at(arm.patterns))
        {
            auto const* const value = _e.at(pattern).node.try_as<flat_int_literal>();
            auto const* const named = _e.at(pattern).node.try_as<flat_enum_value>();
            if (value == nullptr && named == nullptr)
                return unsupported("a switch pattern that is no literal");
            auto const raw
                = value != nullptr ? value->value : _m.at(_m.at(_e.at(pattern).type).cases)[named->case_index].value;
            operands.push_back(u32(raw));
            operands.push_back(u32(b));
        }
    }
    set_merge(merge_kind::selection, merge);
    add(op::switch_, type_ref::none, operands);

    _breaks.push_back(merge);
    for (auto i = isize(0); i < arms.size(); ++i)
    {
        open_block(blocks[i]);
        statements(arms[i].body);
        branch(merge);
    }
    open_block(fallback);
    statements(s.default_body);
    branch(merge);
    _breaks.pop_back();
    open_block(merge);
}

void lowering::declare(local_id id, flat_expr_id value, bool is_mut)
{
    auto const& l = _e.at(id);
    auto const type = value_type(l.type);
    if (!is_valid(type))
        return;
    auto const position = index_of(id);
    if (_out.at(type).kind == type_kind::ray_query)
    {
        _locals[position] = begin_ray_query(id);
        return;
    }
    // EMIT-246: the value the trace gave, which nothing holds in a variable and no let copies
    if (_out.at(type).kind == type_kind::hit_object)
    {
        if (is_mut || !is_valid(value))
            return unsupported("a hit object that is no let of its trace");
        _locals[position] = expr(value);
        return;
    }
    if (l.is_undefined)
    {
        // the prelude's `undefined()`: nothing to evaluate, and a mutable one a variable that holds nothing yet
        _is_variable[position] = is_mut;
        _locals[position] = is_mut ? add_local(type, l.name) : value_ref::none;
        if (!is_mut)
        {
            _out.constants.push_back({.type = type, .is_undefined = true});
            _locals[position] = constant_at(u32(_out.constants.size() - 1));
        }
        describe(id, _locals[position], is_mut);
        return;
    }
    // an array is reached by an index, which only a variable can take
    if (!is_mut && !holds_array(type))
    {
        if (is_valid(value))
            _locals[position] = expr(value);
        describe(id, _locals[position], false);
        return;
    }
    auto const variable = add_local(type, l.name);
    _is_variable[position] = true;
    _locals[position] = variable;
    if (is_valid(value))
        add(op::store, type_ref::none, {variable, expr(value)});
    else if (l.is_zeroed)
        add(op::store, type_ref::none, {variable, null_of(type)});
    describe(id, variable, true);
}

namespace
{
/// The first leaf of a value of `t` that its field `member` holds.
[[nodiscard]] u32 first_leaf_of(program const& p, type_ref t, i32 member)
{
    auto const& info = p.at(t);
    if (info.kind != ir::type_kind::structure)
        return u32(member) * p.leaves_of(info.element);
    auto leaf = u32(0);
    for (auto i = u32(0); i < u32(member); ++i)
        leaf += p.leaves_of(p.members[info.first_member + i].type);
    return leaf;
}
} // namespace

void lowering::describe(local_id id, value_ref held, bool is_variable)
{
    if (!_out.describes_variables || !is_valid(held))
        return;
    // a piece of a split local is that local's value from the piece's first leaf on, though no operation holds the
    // whole, so the types made for it here are the debugger's alone
    _describing = true;
    auto leaf = u32(0);
    while (_e.at(id).source_name.empty() && is_valid(_e.at(id).piece_of))
    {
        auto const whole = _e.at(id).piece_of;
        leaf += first_leaf_of(_out, value_type(_e.at(whole).type), _e.at(id).piece_member);
        id = whole;
        is_variable = false;
    }
    auto const& l = _e.at(id);
    if (l.source_name.empty())
    {
        _describing = false;
        return;
    }
    auto const position = index_of(id);
    // an entry point's parameter is described ahead of every statement, and takes the body's first for its place, as a
    // debugger needs a line and a scope to show it in
    auto const statement = _statement;
    if (auto const body = _e.at(_e.body); _statement == 0xffffffffu && !body.empty())
        _statement = u32(index_of(body[0]));
    // an inlined call's parameter is its callee's variable, placed at a statement of the callee's body
    if (!l.scope_through.empty())
        for (auto s = isize(0); s < _e.stmts.size(); ++s)
            if (_e.stmts[s].inlined_through == l.scope_through)
            {
                _statement = u32(s);
                break;
            }
    if (_variable_of[position] == k_no_element)
    {
        _variable_of[position] = u32(_out.debug_variables.size());
        _out.debug_variables.push_back({.name_first = add_name(l.source_name),
                                        .name_size = u32(l.source_name.size()),
                                        .type = value_type(l.type),
                                        .statement = _statement,
                                        .is_parameter = l.kind == check::local_kind::parameter});
    }
    _out.debug_variables[_variable_of[position]].is_mutable |= is_variable;
    _describing = false;
    u32 const operands[] = {_variable_of[position], u32(held), leaf};
    if (is_variable)
        add(op::debug_declare, type_ref::none, cc::span<u32 const>(operands, 2));
    else
        add(op::debug_value, type_ref::none, cc::span<u32 const>(operands, 3));
    _statement = statement;
}

void lowering::assign(flat_assign const& a)
{
    if (auto const* const member = _e.at(a.place).node.try_as<flat_member>(); member != nullptr && member->is_swizzle())
        return assign_swizzle(*member, a.value);
    auto const target = place_of(a.place);
    if (!target.has_value())
        return unsupported("an assignment to something that is no place");
    store(target.value(), expr(a.value));
}

void lowering::assign_swizzle(flat_member const& swizzled, flat_expr_id value)
{
    auto const target = place_of(swizzled.object);
    if (!target.has_value())
        return unsupported("a swizzle assigned to something that is no place");
    auto const stored = expr(value);
    auto const vector = value_type(target.value().type);
    auto result = load(target.value());
    auto const scalar = scalar_of(vector);
    for (auto i = 0; i < swizzled.letters.count; ++i)
    {
        auto const part = extracted(stored, scalar, u32(i));
        u32 const operands[] = {u32(part), u32(result), u32(swizzled.letters.fields[i])};
        result = add(op::insert, vector, operands);
    }
    store(target.value(), result);
}

cc::optional<cc::string> sgl::ir::lower(emit::impl::plan const& plan, program& out)
{
    auto walk = lowering(plan, out);
    walk.run();
    if (walk.failure().empty())
        return {};
    return walk.failure();
}
