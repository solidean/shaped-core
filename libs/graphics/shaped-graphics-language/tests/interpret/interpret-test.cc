#include "../legalize/flat-test-support.hh"

#include <clean-core/math/bit.hh>

using namespace sgl_test;
using namespace sgl::check;

namespace
{
cc::string run(flat_builder const& b, run_limits const& limits = {})
{
    return dump(interpret(b.m, b.e, test_inputs(b.m), limits));
}
} // namespace

TEST("sgl interpret - the cube's entry points run: a pixel is lit, and a vertex goes through the matrix")
{
    auto const checked = check_sources(read_prelude(), read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl"));
    auto const& m = checked.module;
    REQUIRE(m.entry_points.size() == 2);

    auto const& ps = m.entry_points[1];
    auto pixel = run_inputs{.parameter = zero_value(m, ps.input)};
    // position 4, normal 3, color 3: a normal that faces the key light, and a red face
    pixel.parameter.leaves[5] = scalar::of(1.0f);
    pixel.parameter.leaves[7] = scalar::of(1.0f);
    auto const lit = interpret(m, ps, pixel);
    CHECK(lit.status == run_status::ok);
    REQUIRE(lit.result.leaves.size() == 4);
    CHECK(lit.result.leaves[0].as_float() > 0.25f);
    CHECK(lit.result.leaves[1].as_float() == 0.0f);
    CHECK(lit.result.leaves[3].as_float() == 1.0f);
    CHECK(lit.trace.empty());

    auto const& vs = m.entry_points[0];
    auto vertex = run_inputs{.parameter = zero_value(m, vs.input)};
    vertex.parameter.leaves[0] = scalar::of(2.0f);
    vertex.parameter.leaves[1] = scalar::of(3.0f);
    vertex.parameter.leaves[2] = scalar::of(4.0f);
    // the identity, with a translation of 10 along x in its last column
    auto constants = zero_value(m, flat_builder{.m = m}.type_named("mat4"));
    for (auto const diagonal : {0, 5, 10, 15})
        constants.leaves[diagonal] = scalar::of(1.0f);
    constants.leaves[12] = scalar::of(10.0f);
    vertex.bindings.push_back(constants);
    CHECK(dump(interpret(m, vs, vertex)) == "ok 12 3 4 1 0 0 0 0 0 0");

    // without the binding's value the run says so, and does not read past the inputs
    vertex.bindings.clear();
    CHECK(interpret(m, vs, vertex).status == run_status::type_error);
}

TEST("sgl interpret - operands run left to right, each once, and a block hands its value on")
{
    auto const checked = flat_test_module();
    auto b = float_function(checked.module);
    auto const float_type = b.type_named("float");
    auto const left = b.add_label("left");
    auto const right = b.add_label("right");
    auto const sum = b.call(
        "subtract", {
                        b.block_expr(left, float_type, {b.print(b.literal(1.0)), b.leave(left, b.literal(10.0))}),
                        b.block_expr(right, float_type, {b.print(b.literal(2.0)), b.leave(right, b.literal(4.0))}),
                    });
    b.set_body({b.leave(b.e.root, sum)});
    CHECK(run(b) == "ok 6 | print 1 | print 2");
}

TEST("sgl interpret - and / or skip their right operand, and a call with an effect joins the trace where it runs")
{
    auto const checked = flat_test_module();
    auto b = float_function(checked.module);
    auto const bool_type = b.type_named("bool");
    auto const noisy = [&](f64 tag, bool value)
    {
        auto const label = b.add_label("noisy");
        return b.block_expr(label, bool_type, {b.print(b.literal(tag)), b.leave(label, b.bool_literal(value))});
    };
    b.set_body({
        b.print(b.and_(b.bool_literal(false), noisy(1.0, true))),
        b.print(b.and_(b.bool_literal(true), noisy(2.0, true))),
        b.print(b.or_(b.bool_literal(true), noisy(3.0, false))),
        b.print(b.or_(b.bool_literal(false), noisy(4.0, false))),
        b.print(b.call_with_effect("saturate", {b.literal(7.0)})),
        b.leave(b.e.root, b.literal(0.0)),
    });
    CHECK(run(b) == "ok 0 | print false | print 2 | print true | print true | print 4 | print false | print 1 | print 1");
}

TEST("sgl interpret - a for evaluates its bounds once, and a while meets its condition again after a continue")
{
    auto const checked = flat_test_module();
    auto b = float_function(checked.module);
    auto const int_type = b.type_named("int");
    auto const end = b.var("end", int_type, b.int_literal(2));
    auto const index = b.add_local(local_kind::index, "i", int_type);
    auto const rows = b.add_label("rows");
    auto const n = b.var("n", int_type, b.int_literal(0));
    auto const counting = b.add_label("counting");
    b.set_body({
        end.stmt,
        b.for_(rows, index, b.int_literal(0), b.local(end.local),
               {b.assign(b.local(end.local), b.int_literal(100)), b.print(b.local(index))}),
        n.stmt,
        b.while_(counting, b.call("less_int", {b.local(n.local), b.int_literal(3)}),
                 {
                     b.assign(b.local(n.local), b.call("add_int", {b.local(n.local), b.int_literal(1)})),
                     b.if_(b.call("equal_int", {b.local(n.local), b.int_literal(2)}), {b.continue_(counting)}),
                     b.print(b.local(n.local)),
                 }),
        b.leave(b.e.root, b.literal(0.0)),
    });
    CHECK(run(b) == "ok 0 | print 0 | print 1 | print 1 | print 3");
}

TEST("sgl interpret - a member of a var is assigned in place, and int arithmetic wraps")
{
    auto const checked = flat_test_module();
    auto b = float_function(checked.module);
    auto const float3 = b.type_named("float3");
    auto const s = b.var("s", float3, b.construct(float3, {b.literal(1.0), b.literal(2.0), b.literal(3.0)}));
    b.set_body({
        s.stmt,
        b.assign(b.member(b.local(s.local), "y"), b.literal(20.0)),
        b.print(b.local(s.local)),
        b.print(b.call("add_int", {b.int_literal(2147483647), b.int_literal(1)})),
        b.leave(b.e.root, b.member(b.local(s.local), "y")),
    });
    CHECK(run(b) == "ok 20 | print 1 20 3 | print -2147483648");
}

TEST("sgl interpret - a conversion truncates and saturates a float, and keeps the bits between int and uint")
{
    auto const checked = flat_test_module();
    auto b = float_function(checked.module);
    b.set_body({
        b.print(b.call("convert_float_to_int", {b.literal(-3.75)})),
        b.print(b.call("convert_float_to_int", {b.literal(1e20)})),
        b.print(b.call("convert_float_to_uint", {b.literal(-2.0)})),
        b.print(b.call("convert_int_to_uint", {b.int_literal(-1)})),
        b.print(b.call("convert_int_to_float", {b.int_literal(7)})),
        b.leave(b.e.root, b.literal(0.0)),
    });
    CHECK(run(b) == "ok 0 | print -3 | print 2147483647 | print 0u | print 4294967295u | print 7");
}

TEST("sgl interpret - its choice where a conversion is unspecified, and the bits of a uint past 2^31")
{
    auto const checked = flat_test_module();
    auto b = float_function(checked.module);
    auto const nan = cc::bit_cast<f64>(u64(0x7ff8000000000000ull));
    b.set_body({
        // A NaN is unspecified on the targets (CHK-197), and the interpreter picks 0.
        b.print(b.call("convert_float_to_int", {b.literal(nan)})),
        b.print(b.call("convert_float_to_uint", {b.literal(nan)})),
        b.print(b.call("convert_float_to_int", {b.literal(-1e20)})),
        // 4294967294 as an int is -2: the bits stay.
        b.print(b.call("convert_uint_to_int", {b.call("convert_int_to_uint", {b.int_literal(-2)})})),
        b.leave(b.e.root, b.literal(0.0)),
    });
    CHECK(run(b) == "ok 0 | print 0 | print 0u | print -2147483648 | print -2");
}

TEST("sgl interpret - every way a run ends without a result is a status")
{
    auto const checked = flat_test_module();
    auto const float_type = flat_builder{.m = checked.module}.type_named("float");

    SECTION("a loop nothing leaves runs out of fuel, and keeps what it printed")
    {
        auto b = float_function(checked.module);
        b.set_body({b.loop(b.add_label("forever"), {b.print(b.literal(1.0))})});
        auto const o = interpret(b.m, b.e, test_inputs(b.m), {.fuel = 10});
        CHECK(o.status == run_status::out_of_fuel);
        CHECK(!o.trace.empty());
        CHECK(o.trace.size() < 10);
    }
    SECTION("a body without a leave falls off the end, and so does a block expression")
    {
        auto b = float_function(checked.module);
        b.set_body({b.print(b.literal(1.0))});
        CHECK(run(b) == "fell-off-the-end (the function) | print 1");

        auto c = float_function(checked.module);
        c.set_body({c.leave(c.e.root, c.block_expr(c.add_label("v"), float_type, {c.print(c.literal(1.0))}))});
        CHECK(run(c) == "fell-off-the-end (a block expression) | print 1");
    }
    SECTION("a var is read only after something was assigned to it")
    {
        auto b = float_function(checked.module);
        auto const x = b.var("x", float_type);
        b.set_body({x.stmt, b.leave(b.e.root, b.local(x.local))});
        CHECK(run(b) == "uninitialized-read (x)");
    }
    SECTION("what is ill typed or ill formed is a type error, never an assertion")
    {
        auto b = float_function(checked.module);
        b.set_body({b.if_(b.literal(1.0), {})});
        CHECK(run(b) == "type-error (a condition or a logical operand that is no bool)");

        auto c = float_function(checked.module);
        auto const fixed = c.let("fixed", c.literal(1.0));
        c.set_body({fixed.stmt, c.assign(c.local(fixed.local), c.literal(2.0))});
        CHECK(run(c) == "type-error (an assignment to what is no mutable local)");

        auto d = float_function(checked.module);
        d.set_body({d.break_()});
        CHECK(run(d) == "type-error (an exit that nothing encloses)");

        // ids that name nothing, a list that reaches outside the tree, and an expression that holds itself
        auto g = float_function(checked.module);
        g.set_body({g.print(flat_expr_id(1234)), g.leave(g.e.root, g.literal(0.0))});
        CHECK(interpret(g.m, g.e, test_inputs(g.m)).status == run_status::type_error);
        g.e.body = {.first = 7, .count = 900};
        CHECK(interpret(g.m, g.e, test_inputs(g.m)).status == run_status::type_error);
        CHECK(find_core_violation(g.e).has_value());
        CHECK(!dump_entry_point(g.m, legalize(g.m, g.e)).empty());

        auto h = float_function(checked.module);
        auto const self = h.not_(flat_expr_id(h.e.exprs.size()));
        h.set_body({h.leave(h.e.root, self)});
        CHECK(interpret(h.m, h.e, test_inputs(h.m)).status == run_status::type_error);
        CHECK(!dump_entry_point(h.m, legalize(h.m, h.e)).empty());
    }
}

TEST("sgl core - the first violation says why a tree is not core")
{
    auto const checked = flat_test_module();
    auto const reason_of = [](flat_builder const& b)
    {
        auto const violation = find_core_violation(b.e);
        return violation.has_value() ? violation.value().reason : cc::string("core");
    };
    auto const bool_type = flat_builder{.m = checked.module}.type_named("bool");

    auto a = float_function(checked.module);
    a.set_body({a.block(a.add_label("b"), {}), a.return_(a.literal(0.0))});
    CHECK(reason_of(a) == "the block $b, which must be a once or nothing at all");

    auto b = float_function(checked.module);
    b.set_body({b.leave(b.e.root, b.literal(0.0))});
    CHECK(reason_of(b) == "a leave of $root, which must be a break, a continue or a return");

    auto c = float_function(checked.module);
    auto const v = c.add_label("v");
    c.set_body({c.return_(c.block_expr(v, c.type_named("float"), {c.leave(v, c.literal(1.0))}))});
    CHECK(reason_of(c) == "the block expression $v, since a core expression holds no statement");

    auto d = float_function(checked.module);
    auto const loop = d.add_label("l");
    d.set_body({d.loop(loop, {d.once({d.continue_(loop)})}), d.return_(d.literal(0.0))});
    CHECK(reason_of(d) == "a continue of $l that would cross a once");

    auto e = float_function(checked.module);
    auto const outer = e.add_label("outer");
    auto const inner = e.add_label("inner");
    e.set_body({e.loop(outer, {e.loop(inner, {e.continue_(outer)})}), e.return_(e.literal(0.0))});
    CHECK(reason_of(e) == "a continue of $outer, which is not the innermost loop");

    auto f = float_function(checked.module);
    f.set_body({f.print(f.and_(f.bool_literal(true), f.call_with_effect("less", {f.literal(1.0), f.literal(2.0)}))),
                f.return_(f.literal(0.0))});
    CHECK(reason_of(f) == "an `and` whose right operand has an effect, which must be an `if`");

    auto g = float_function(checked.module);
    g.set_body({g.break_(), g.return_(g.literal(0.0))});
    CHECK(reason_of(g) == "a break outside every once and every loop");

    // what IS core: a return at any depth, a break in a once, a continue directly in its loop, `&&` over a pure operand
    auto h = float_function(checked.module);
    auto const l = h.add_label("l");
    auto const flag = h.var("flag", bool_type);
    h.set_body({
        flag.stmt,
        h.loop(l, {h.once({h.if_(h.and_(h.bool_literal(true), h.not_(h.bool_literal(false))), {h.break_()}),
                           h.return_(h.literal(1.0))}),
                   h.continue_(l)}),
    });
    CHECK(reason_of(h) == "core");
}
