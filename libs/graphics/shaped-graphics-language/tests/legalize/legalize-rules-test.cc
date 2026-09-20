#include "flat-test-support.hh"

using namespace sgl_test;
using namespace sgl::check;

namespace
{
/// A function body under construction with three conditions in scope: `c`, `d` and `e` are immutable bools.
/// Their three `let`s are the same in every test, so a dump here starts after them.
struct rule_fixture
{
    checked_sources checked = flat_test_module();
    flat_builder b = float_function(checked.module);
    cc::vector<flat_stmt_id> head;
    local_id c_local = local_id::none;
    local_id d_local = local_id::none;
    local_id e_local = local_id::none;

    rule_fixture()
    {
        auto const declare = [&](cc::string_view name, f64 limit)
        {
            auto const value = b.call(builtin::less, {b.member(b.local(local_id(0)), "a"), b.literal(limit)});
            auto const declared = b.let(name, value);
            head.push_back(declared.stmt);
            return declared.local;
        };
        // p.a is 0.25 in `test_inputs`: c is true, d is false, e is true
        c_local = declare("c", 0.5);
        d_local = declare("d", 0.125);
        e_local = declare("e", 1.0);
    }

    flat_expr_id c() { return b.local(c_local); }
    flat_expr_id d() { return b.local(d_local); }
    flat_expr_id e() { return b.local(e_local); }
    flat_expr_id f(f64 value) { return b.literal(value); }
    flat_stmt_id print(f64 value) { return b.print(b.literal(value)); }
    type_id float_type() { return b.type_of(builtin::scalar_float); }
    type_id bool_type() { return b.type_of(builtin::boolean); }

    /// Ends the body in `leave $root 0.0`.
    void finish(cc::span<flat_stmt_id const> body)
    {
        auto all = head;
        all.push_back_range(body);
        all.push_back(b.leave(b.e.root, b.literal(0.0)));
        b.set_body(all);
    }

    static cc::string without_head(cc::string text)
    {
        for (auto i = 0; i < 3; ++i)
            text = cc::string(cc::string_view(text).subview({.start = text.find('\n') + 1, .end = text.size()}));
        return text;
    }

    cc::string structured() const { return without_head(body_dump(checked.module, b.e)); }

    /// Legalizes, and checks what every rule test wants to know: the result is core and it behaves the same.
    cc::string core() const
    {
        auto const& m = checked.module;
        auto const legal = legalize(m, b.e);
        auto const violation = find_core_violation(legal);
        CHECK(!violation.has_value());
        auto const expected = interpret(m, b.e, test_inputs(m));
        CHECK(expected.status == run_status::ok);
        CHECK(dump(interpret(m, legal, test_inputs(m))) == dump(expected));
        return without_head(body_dump(m, legal));
    }
};
} // namespace

TEST("sgl legalize - X2: guard clauses become nested else branches, and the label is gone")
{
    auto t = rule_fixture();
    auto const block = t.b.add_label("b");
    t.finish({
        t.b.block(block,
                  {
                      t.b.if_(t.d(), {t.print(1.0), t.b.leave(block)}),
                      t.b.if_(t.c(), {t.print(2.0), t.b.leave(block)}),
                      t.print(3.0),
                  }),
        t.print(4.0),
    });
    CHECK(t.structured()
          == "  (block $b\n"
             "    (if (local d : bool)\n"
             "      (then\n"
             "        (print (lit 1.0 : float))\n"
             "        (leave $b)))\n"
             "    (if (local c : bool)\n"
             "      (then\n"
             "        (print (lit 2.0 : float))\n"
             "        (leave $b)))\n"
             "    (print (lit 3.0 : float)))\n"
             "  (print (lit 4.0 : float))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (if (local d : bool)\n"
             "    (then\n"
             "      (print (lit 1.0 : float)))\n"
             "    (else\n"
             "      (if (local c : bool)\n"
             "        (then\n"
             "          (print (lit 2.0 : float)))\n"
             "        (else\n"
             "          (print (lit 3.0 : float))))))\n"
             "  (print (lit 4.0 : float))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - X2 with a value: every leave assigns the result, and no once is needed")
{
    auto t = rule_fixture();
    auto const block = t.b.add_label("pick");
    auto const value = t.b.block_expr(block, t.float_type(),
                                      {
                                          t.b.if_(t.d(), {t.b.leave(block, t.f(1.0))}),
                                          t.b.if_(t.c(), {t.print(2.0), t.b.leave(block, t.f(2.0))}),
                                          t.b.leave(block, t.f(3.0)),
                                      });
    t.finish({t.b.print(value)});
    CHECK(t.structured()
          == "  (print (block $pick\n"
             "      (if (local d : bool)\n"
             "        (then\n"
             "          (leave $pick (lit 1.0 : float))))\n"
             "      (if (local c : bool)\n"
             "        (then\n"
             "          (print (lit 2.0 : float))\n"
             "          (leave $pick (lit 2.0 : float))))\n"
             "      (leave $pick (lit 3.0 : float)) : float))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (var:temporary pick_result : float)\n"
             "  (if (local d : bool)\n"
             "    (then\n"
             "      (assign (local pick_result : float) = (lit 1.0 : float)))\n"
             "    (else\n"
             "      (if (local c : bool)\n"
             "        (then\n"
             "          (print (lit 2.0 : float))\n"
             "          (assign (local pick_result : float) = (lit 2.0 : float)))\n"
             "        (else\n"
             "          (assign (local pick_result : float) = (lit 3.0 : float))))))\n"
             "  (print (local pick_result : float))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - X2: a block with a single exit at its end is its expression")
{
    auto t = rule_fixture();
    auto const block = t.b.add_label("v");
    auto const value = t.b.block_expr(block, t.float_type(), {t.print(1.0), t.b.leave(block, t.f(2.0))});
    auto const bare = t.b.add_label("w");
    auto const bare_value = t.b.block_expr(bare, t.float_type(), {t.b.leave(bare, t.f(5.0))});
    t.finish({t.b.print(t.b.call(builtin::add, {value, bare_value}))});
    CHECK(t.structured()
          == "  (print (call add (block $v\n"
             "      (print (lit 1.0 : float))\n"
             "      (leave $v (lit 2.0 : float)) : float) (block $w\n"
             "      (leave $w (lit 5.0 : float)) : float) : float))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (print (lit 1.0 : float))\n"
             "  (print (call add (lit 2.0 : float) (lit 5.0 : float) : float))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - X3: a leave of the innermost loop is a break")
{
    auto t = rule_fixture();
    auto const loop = t.b.add_label("l");
    t.finish({t.b.loop(loop, {t.print(1.0), t.b.if_(t.c(), {t.b.leave(loop)}), t.print(2.0)})});
    CHECK(t.structured()
          == "  (loop $l\n"
             "    (print (lit 1.0 : float))\n"
             "    (if (local c : bool)\n"
             "      (then\n"
             "        (leave $l)))\n"
             "    (print (lit 2.0 : float)))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (loop $l\n"
             "    (print (lit 1.0 : float))\n"
             "    (if (local c : bool)\n"
             "      (then\n"
             "        (break)))\n"
             "    (print (lit 2.0 : float)))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - X4: a block with a leave outside tail position is a once")
{
    auto t = rule_fixture();
    auto const block = t.b.add_label("b");
    t.finish({t.b.block(block, {
                                   t.b.if_(t.c(), {t.b.if_(t.d(), {t.b.leave(block)}), t.print(1.0)}),
                                   t.print(2.0),
                               })});
    CHECK(t.structured()
          == "  (block $b\n"
             "    (if (local c : bool)\n"
             "      (then\n"
             "        (if (local d : bool)\n"
             "          (then\n"
             "            (leave $b)))\n"
             "        (print (lit 1.0 : float))))\n"
             "    (print (lit 2.0 : float)))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (once\n"
             "    (if (local c : bool)\n"
             "      (then\n"
             "        (if (local d : bool)\n"
             "          (then\n"
             "            (break)))\n"
             "        (print (lit 1.0 : float))))\n"
             "    (print (lit 2.0 : float)))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - X5: a leave that crosses one loop sets a flag, which is tested once behind the loop")
{
    auto t = rule_fixture();
    auto const block = t.b.add_label("found");
    auto const loop = t.b.add_label("l");
    t.finish({t.b.block(block,
                        {
                            t.b.loop(loop, {t.print(1.0), t.b.if_(t.c(), {t.b.leave(block)}), t.b.leave(loop)}),
                            t.print(2.0),
                        }),
              t.print(3.0)});
    CHECK(t.structured()
          == "  (block $found\n"
             "    (loop $l\n"
             "      (print (lit 1.0 : float))\n"
             "      (if (local c : bool)\n"
             "        (then\n"
             "          (leave $found)))\n"
             "      (leave $l))\n"
             "    (print (lit 2.0 : float)))\n"
             "  (print (lit 3.0 : float))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (var:temporary found_left : bool = (lit false : bool))\n"
             "  (once\n"
             "    (loop $l\n"
             "      (print (lit 1.0 : float))\n"
             "      (if (local c : bool)\n"
             "        (then\n"
             "          (assign (local found_left : bool) = (lit true : bool))\n"
             "          (break)))\n"
             "      (break))\n"
             "    (if (local found_left : bool)\n"
             "      (then\n"
             "        (break)))\n"
             "    (print (lit 2.0 : float)))\n"
             "  (print (lit 3.0 : float))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - X5: a leave that crosses two loops is tested behind each of them")
{
    auto t = rule_fixture();
    auto const block = t.b.add_label("found");
    auto const outer = t.b.add_label("rows");
    auto const inner = t.b.add_label("columns");
    t.finish({t.b.block(block, {
                                   t.b.loop(outer,
                                            {
                                                t.b.loop(inner, {t.b.if_(t.c(), {t.b.leave(block)}), t.b.leave(inner)}),
                                                t.print(1.0),
                                                t.b.leave(outer),
                                            }),
                                   t.print(2.0),
                               })});
    CHECK(t.structured()
          == "  (block $found\n"
             "    (loop $rows\n"
             "      (loop $columns\n"
             "        (if (local c : bool)\n"
             "          (then\n"
             "            (leave $found)))\n"
             "        (leave $columns))\n"
             "      (print (lit 1.0 : float))\n"
             "      (leave $rows))\n"
             "    (print (lit 2.0 : float)))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (var:temporary found_left : bool = (lit false : bool))\n"
             "  (once\n"
             "    (loop $rows\n"
             "      (loop $columns\n"
             "        (if (local c : bool)\n"
             "          (then\n"
             "            (assign (local found_left : bool) = (lit true : bool))\n"
             "            (break)))\n"
             "        (break))\n"
             "      (if (local found_left : bool)\n"
             "        (then\n"
             "          (break)))\n"
             "      (print (lit 1.0 : float))\n"
             "      (break))\n"
             "    (if (local found_left : bool)\n"
             "      (then\n"
             "        (break)))\n"
             "    (print (lit 2.0 : float)))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - X5: a continue never crosses a once")
{
    auto t = rule_fixture();
    auto const loop = t.b.add_label("l");
    auto const block = t.b.add_label("b");
    auto const index = t.b.add_local(local_kind::index, "i", t.b.type_of(builtin::scalar_int));
    t.finish({t.b.for_(loop, index, t.b.int_literal(0), t.b.int_literal(2),
                       {
                           t.b.block(block,
                                     {
                                         t.b.if_(t.c(), {t.b.if_(t.d(), {t.b.leave(block)}),
                                                         t.b.if_(t.e(), {t.b.continue_(loop)}), t.print(1.0)}),
                                         t.print(2.0),
                                     }),
                           t.print(3.0),
                       })});
    CHECK(t.structured()
          == "  (for $l i in (lit 0 : int) ..< (lit 2 : int)\n"
             "    (block $b\n"
             "      (if (local c : bool)\n"
             "        (then\n"
             "          (if (local d : bool)\n"
             "            (then\n"
             "              (leave $b)))\n"
             "          (if (local e : bool)\n"
             "            (then\n"
             "              (continue $l)))\n"
             "          (print (lit 1.0 : float))))\n"
             "      (print (lit 2.0 : float)))\n"
             "    (print (lit 3.0 : float)))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (for $l i in (lit 0 : int) ..< (lit 2 : int)\n"
             "    (var:temporary l_continued : bool = (lit false : bool))\n"
             "    (once\n"
             "      (if (local c : bool)\n"
             "        (then\n"
             "          (if (local d : bool)\n"
             "            (then\n"
             "              (break)))\n"
             "          (if (local e : bool)\n"
             "            (then\n"
             "              (assign (local l_continued : bool) = (lit true : bool))\n"
             "              (break)))\n"
             "          (print (lit 1.0 : float))))\n"
             "      (print (lit 2.0 : float)))\n"
             "    (if (local l_continued : bool)\n"
             "      (then\n"
             "        (continue $l)))\n"
             "    (print (lit 3.0 : float)))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - E2: an operand is pinned only when the block to its right could change or reorder it")
{
    auto t = rule_fixture();
    auto const float_type = t.float_type();
    auto const touched = t.b.var("touched", float_type, t.f(1.0));
    auto const untouched = t.b.var("untouched", float_type, t.f(2.0));
    auto const fixed = t.b.let("fixed", t.f(3.0));

    auto const assigning_block = [&](cc::string_view name)
    {
        auto const block = t.b.add_label(name);
        return t.b.block_expr(block, float_type,
                              {
                                  t.b.assign(t.b.local(touched.local), t.f(10.0)),
                                  t.b.if_(t.d(), {t.b.leave(block, t.f(20.0))}),
                                  t.b.leave(block, t.f(30.0)),
                              });
    };
    auto const float4 = t.b.type_of(builtin::float4);
    // free: a literal, an immutable local, and a var the block leaves alone
    auto const free_operands
        = t.b.construct(float4, {t.f(0.5), t.b.local(fixed.local), t.b.local(untouched.local), assigning_block("v")});
    // pinned: a var the block assigns, and a call with an effect; the literal between them stays
    auto const pinned_operands
        = t.b.construct(float4, {t.b.local(touched.local), t.b.call_with_effect(builtin::saturate, {t.f(4.0)}),
                                 t.f(0.5), assigning_block("w")});
    t.finish({touched.stmt, untouched.stmt, fixed.stmt, t.b.print(free_operands), t.b.print(pinned_operands)});
    CHECK(t.structured()
          == "  (var touched : float = (lit 1.0 : float))\n"
             "  (var untouched : float = (lit 2.0 : float))\n"
             "  (let fixed : float = (lit 3.0 : float))\n"
             "  (print (construct (lit 0.5 : float) (local fixed : float) (local untouched : float) (block $v\n"
             "      (assign (local touched : float) = (lit 10.0 : float))\n"
             "      (if (local d : bool)\n"
             "        (then\n"
             "          (leave $v (lit 20.0 : float))))\n"
             "      (leave $v (lit 30.0 : float)) : float) : float4))\n"
             "  (print (construct (local touched : float) (call:effect saturate (lit 4.0 : float) : float) (lit 0.5 : "
             "float) (block $w\n"
             "      (assign (local touched : float) = (lit 10.0 : float))\n"
             "      (if (local d : bool)\n"
             "        (then\n"
             "          (leave $w (lit 20.0 : float))))\n"
             "      (leave $w (lit 30.0 : float)) : float) : float4))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (var touched : float = (lit 1.0 : float))\n"
             "  (var untouched : float = (lit 2.0 : float))\n"
             "  (let fixed : float = (lit 3.0 : float))\n"
             "  (var:temporary v_result : float)\n"
             "  (assign (local touched : float) = (lit 10.0 : float))\n"
             "  (if (local d : bool)\n"
             "    (then\n"
             "      (assign (local v_result : float) = (lit 20.0 : float)))\n"
             "    (else\n"
             "      (assign (local v_result : float) = (lit 30.0 : float))))\n"
             "  (print (construct (lit 0.5 : float) (local fixed : float) (local untouched : float) (local v_result : "
             "float) : float4))\n"
             "  (let:temporary touched_before : float = (local touched : float))\n"
             "  (let:temporary saturate_value : float = (call:effect saturate (lit 4.0 : float) : float))\n"
             "  (var:temporary w_result : float)\n"
             "  (assign (local touched : float) = (lit 10.0 : float))\n"
             "  (if (local d : bool)\n"
             "    (then\n"
             "      (assign (local w_result : float) = (lit 20.0 : float)))\n"
             "    (else\n"
             "      (assign (local w_result : float) = (lit 30.0 : float))))\n"
             "  (print (construct (local touched_before : float) (local saturate_value : float) (lit 0.5 : float) "
             "(local w_result : float) : float4))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - E3: and / or stay operators over a right side without effect, and become an if otherwise")
{
    auto t = rule_fixture();
    auto const block = t.b.add_label("v");
    auto const noisy = t.b.block_expr(block, t.bool_type(), {t.print(1.0), t.b.leave(block, t.e())});
    t.finish({
        t.b.print(t.b.and_(t.c(), t.b.not_(t.d()))),
        t.b.print(t.b.and_(t.c(), noisy)),
        t.b.print(t.b.or_(t.d(), t.b.call_with_effect(builtin::less, {t.f(1.0), t.f(2.0)}))),
    });
    CHECK(t.structured()
          == "  (print (and (local c : bool) (not (local d : bool) : bool) : bool))\n"
             "  (print (and (local c : bool) (block $v\n"
             "      (print (lit 1.0 : float))\n"
             "      (leave $v (local e : bool)) : bool) : bool))\n"
             "  (print (or (local d : bool) (call:effect less (lit 1.0 : float) (lit 2.0 : float) : bool) : bool))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (print (and (local c : bool) (not (local d : bool) : bool) : bool))\n"
             "  (var:temporary and_result : bool = (local c : bool))\n"
             "  (if (local and_result : bool)\n"
             "    (then\n"
             "      (print (lit 1.0 : float))\n"
             "      (assign (local and_result : bool) = (local e : bool))))\n"
             "  (print (local and_result : bool))\n"
             "  (var:temporary or_result : bool = (local d : bool))\n"
             "  (if (not (local or_result : bool) : bool)\n"
             "    (then\n"
             "      (assign (local or_result : bool) = (call:effect less (lit 1.0 : float) (lit 2.0 : float) : "
             "bool))))\n"
             "  (print (local or_result : bool))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - E4: a condition with statements moves to the top of the loop, where a continue still meets it")
{
    auto t = rule_fixture();
    auto const int_type = t.b.type_of(builtin::scalar_int);
    auto const n = t.b.var("n", int_type, t.b.int_literal(0));
    auto const loop = t.b.add_label("l");
    auto const block = t.b.add_label("v");
    auto const condition
        = t.b.block_expr(block, t.bool_type(),
                         {
                             t.b.print(t.b.local(n.local)),
                             t.b.if_(t.d(), {t.b.leave(block, t.b.bool_literal(false))}),
                             t.b.leave(block, t.b.call(builtin::less_int, {t.b.local(n.local), t.b.int_literal(2)})),
                         });
    t.finish({n.stmt, t.b.while_(loop, condition,
                                 {
                                     t.b.assign(t.b.local(n.local),
                                                t.b.call(builtin::add_int, {t.b.local(n.local), t.b.int_literal(1)})),
                                     t.b.if_(t.c(), {t.b.continue_(loop)}),
                                     t.print(9.0),
                                 })});
    CHECK(t.structured()
          == "  (var n : int = (lit 0 : int))\n"
             "  (while $l (block $v\n"
             "      (print (local n : int))\n"
             "      (if (local d : bool)\n"
             "        (then\n"
             "          (leave $v (lit false : bool))))\n"
             "      (leave $v (call less_int (local n : int) (lit 2 : int) : bool)) : bool)\n"
             "    (assign (local n : int) = (call add_int (local n : int) (lit 1 : int) : int))\n"
             "    (if (local c : bool)\n"
             "      (then\n"
             "        (continue $l)))\n"
             "    (print (lit 9.0 : float)))\n"
             "  (leave $root (lit 0.0 : float))\n");
    CHECK(t.core()
          == "  (var n : int = (lit 0 : int))\n"
             "  (loop $l\n"
             "    (var:temporary v_result : bool)\n"
             "    (print (local n : int))\n"
             "    (if (local d : bool)\n"
             "      (then\n"
             "        (assign (local v_result : bool) = (lit false : bool)))\n"
             "      (else\n"
             "        (assign (local v_result : bool) = (call less_int (local n : int) (lit 2 : int) : bool))))\n"
             "    (if (not (local v_result : bool) : bool)\n"
             "      (then\n"
             "        (break)))\n"
             "    (assign (local n : int) = (call add_int (local n : int) (lit 1 : int) : int))\n"
             "    (if (local c : bool)\n"
             "      (then\n"
             "        (continue $l)))\n"
             "    (print (lit 9.0 : float)))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - the end of a for is pinned when the body could change it")
{
    auto t = rule_fixture();
    auto const int_type = t.b.type_of(builtin::scalar_int);
    auto const count = t.b.var("count", int_type, t.b.int_literal(2));
    auto const loop = t.b.add_label("l");
    auto const index = t.b.add_local(local_kind::index, "i", int_type);
    t.finish(
        {count.stmt, t.b.for_(loop, index, t.b.int_literal(0), t.b.local(count.local),
                              {t.b.assign(t.b.local(count.local), t.b.int_literal(9)), t.b.print(t.b.local(index))})});
    CHECK(t.core()
          == "  (var count : int = (lit 2 : int))\n"
             "  (let:temporary i_end : int = (local count : int))\n"
             "  (for $l i in (lit 0 : int) ..< (local i_end : int)\n"
             "    (assign (local count : int) = (lit 9 : int))\n"
             "    (print (local i : int)))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - a once and a break of the input are a block and a leave, so a core tree with a return stays as it "
     "is")
{
    auto t = rule_fixture();
    auto all = t.head;
    all.push_back(t.b.once({t.b.if_(t.c(), {t.b.break_()}), t.print(1.0)}));
    all.push_back(t.b.return_(t.f(0.0)));
    t.b.set_body(all);
    CHECK(is_core(t.b.e));
    CHECK(legalize(t.checked.module, t.b.e) == t.b.e);

    // the same once under a block expression is legalized again, and the once is no longer needed
    auto u = rule_fixture();
    auto const block = u.b.add_label("v");
    auto const value = u.b.block_expr(
        block, u.float_type(), {u.b.once({u.b.if_(u.c(), {u.b.break_()}), u.print(1.0)}), u.b.leave(block, u.f(2.0))});
    u.finish({u.b.print(value)});
    CHECK(u.core()
          == "  (if (not (local c : bool) : bool)\n"
             "    (then\n"
             "      (print (lit 1.0 : float))))\n"
             "  (print (lit 2.0 : float))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - X6: a block that ends in a loop is left by leaving the loop, so `break value` costs no once")
{
    auto t = rule_fixture();
    auto const found = t.b.add_label("found");
    auto const rows = t.b.add_label("rows");
    auto const weight = t.b.var("weight", t.float_type(), t.f(1.0));
    auto const shade = t.b.let(
        "shade",
        t.b.block_expr(found, t.float_type(),
                       {
                           t.b.loop(rows,
                                    {
                                        t.b.assign(t.b.local(weight.local),
                                                   t.b.call(builtin::multiply, {t.b.local(weight.local), t.f(0.5)})),
                                        t.b.if_(t.b.call(builtin::less, {t.b.local(weight.local), t.f(0.25)}),
                                                {t.b.leave(found, t.b.local(weight.local))}),
                                        t.b.if_(t.d(), {t.b.leave(found, t.f(0.0))}),
                                    }),
                       }));
    t.finish({weight.stmt, shade.stmt, t.b.print(t.b.local(shade.local))});
    CHECK(t.core()
          == "  (var weight : float = (lit 1.0 : float))\n"
             "  (var:temporary found_result : float)\n"
             "  (loop $rows\n"
             "    (assign (local weight : float) = (call multiply (local weight : float) (lit 0.5 : float) : float))\n"
             "    (if (call less (local weight : float) (lit 0.25 : float) : bool)\n"
             "      (then\n"
             "        (assign (local found_result : float) = (local weight : float))\n"
             "        (break)))\n"
             "    (if (local d : bool)\n"
             "      (then\n"
             "        (assign (local found_result : float) = (lit 0.0 : float))\n"
             "        (break))))\n"
             "  (let shade : float = (local found_result : float))\n"
             "  (print (local shade : float))\n"
             "  (return (lit 0.0 : float))\n");
}

TEST("sgl legalize - X6 keeps away from a block that goes on behind its loop")
{
    auto t = rule_fixture();
    auto const found = t.b.add_label("found");
    auto const rows = t.b.add_label("rows");
    t.finish({
        t.b.block(found,
                  {
                      t.b.loop(rows, {t.b.if_(t.c(), {t.b.leave(found)}), t.b.leave(rows)}),
                      t.print(1.0),
                  }),
        t.print(2.0),
    });
    CHECK(t.core()
          == "  (var:temporary found_left : bool = (lit false : bool))\n"
             "  (once\n"
             "    (loop $rows\n"
             "      (if (local c : bool)\n"
             "        (then\n"
             "          (assign (local found_left : bool) = (lit true : bool))\n"
             "          (break)))\n"
             "      (break))\n"
             "    (if (local found_left : bool)\n"
             "      (then\n"
             "        (break)))\n"
             "    (print (lit 1.0 : float)))\n"
             "  (print (lit 2.0 : float))\n"
             "  (return (lit 0.0 : float))\n");
}
