#include "random-program.hh"

#include <clean-core/math/random.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

using namespace sgl::check;

namespace
{
using namespace sgl_test;

struct generator
{
    flat_builder b;
    cc::random rng;
    program_shape shape;
    int nodes_left = 0;

    type_id float_type = type_id::none;
    type_id int_type = type_id::none;
    type_id bool_type = type_id::none;
    type_id float3_type = type_id::none;
    /// `store` of the test module, whose `data` and `counts` the program reads and writes; `none` where there is none.
    symbol_id store = symbol_id::none;

    struct visible
    {
        local_id local = local_id::none;
        type_id type = type_id::none;
        bool is_assignable = false;
    };
    /// The locals a statement generated now may name; a list truncates it back when it ends.
    cc::vector<visible> scope;

    struct target
    {
        label_id label = label_id::none;
        /// `none` for a block statement and a loop, which a leave gives no value.
        type_id value_type = type_id::none;
        bool is_loop = false;
    };
    /// The enclosing blocks and loops, outermost first; the root is the first.
    cc::vector<target> targets;

    int pick(int count) { return rng.uniform(0, count - 1); }
    bool chance(int percent) { return pick(100) < percent; }

    type_id any_type()
    {
        auto const r = pick(3);
        return r == 0 ? float_type : (r == 1 ? int_type : bool_type);
    }

    // ---- expressions ------------------------------------------------------------------------------------------------

    flat_expr_id literal(type_id type)
    {
        if (type == float_type)
        {
            f64 const values[] = {0.5, 1.0, 1.5, 2.0, -1.0, 0.25};
            return b.literal(values[pick(6)]);
        }
        if (type == int_type)
            return b.int_literal(pick(7) - 2);
        return b.bool_literal(chance(50));
    }

    // ---- buffers ----------------------------------------------------------------------------------------------------

    [[nodiscard]] bool has_buffer_of(type_id type) const
    {
        return is_valid(store) && (type == float_type || type == int_type);
    }

    /// `store.data` for a float, `store.counts` for an int.
    flat_expr_id buffer_of(type_id type) { return b.binding_member(store, type == float_type ? 0 : 1); }

    /// An index that is always in range: a literal, or a block that prints or stores on its way to one.
    flat_expr_id element_index(int depth)
    {
        auto const in_range = [&] { return b.int_literal(pick(k_store_elements)); };
        if (depth <= 0 || !chance(35))
            return in_range();
        auto const label = b.add_label("k");
        auto body = cc::vector<flat_stmt_id>();
        if (chance(50))
            body.push_back(b.print(leaf(any_type())));
        else
            body.push_back(store_statement(0));
        body.push_back(b.leave(label, in_range()));
        return b.block_expr(label, int_type, body);
    }

    flat_expr_id buffer_read(type_id type, int depth)
    {
        return b.buffer_element(buffer_of(type), element_index(depth));
    }

    /// `data[i] = …` or `counts[i] = …`, the index a block now and then, which EVAL-14 evaluates before the value.
    flat_stmt_id store_statement(int depth)
    {
        auto const type = chance(50) ? float_type : int_type;
        auto const place = b.buffer_element(buffer_of(type), element_index(depth));
        return b.assign(place, expr(type, depth));
    }

    /// A read of an element to the left of a block that stores to that same buffer: the operand rule E2 must pin it.
    flat_expr_id racy_element(type_id type)
    {
        auto const read = b.buffer_element(buffer_of(type), b.int_literal(pick(k_store_elements)));
        auto const label = b.add_label("r");
        auto body = cc::vector<flat_stmt_id>();
        body.push_back(b.assign(b.buffer_element(buffer_of(type), b.int_literal(pick(k_store_elements))), leaf(type)));
        body.push_back(b.leave(label, leaf(type)));
        auto const block = b.block_expr(label, type, body);
        return b.call(type == float_type ? "add" : "add_int", {read, block});
    }

    flat_expr_id leaf(type_id type)
    {
        auto candidates = cc::vector<local_id>();
        for (auto const& v : scope)
            if (v.type == type)
                candidates.push_back(v.local);
        if (!candidates.empty() && chance(60))
            return b.local(candidates[pick(int(candidates.size()))]);
        if (has_buffer_of(type) && chance(15))
            return b.buffer_element(buffer_of(type), b.int_literal(pick(k_store_elements)));
        if (type == float_type && chance(30))
            return b.member(b.local(local_id(0)), chance(50) ? "a" : "b");
        if (type == float_type && chance(30))
            if (auto const s = any_struct_var(); is_valid(s))
                return b.member(b.local(s), pick(3));
        return literal(type);
    }

    local_id any_struct_var()
    {
        auto candidates = cc::vector<local_id>();
        for (auto const& v : scope)
            if (v.type == float3_type)
                candidates.push_back(v.local);
        return candidates.empty() ? local_id::none : candidates[pick(int(candidates.size()))];
    }

    flat_expr_id block_expr(type_id type, int depth)
    {
        auto const label = b.add_label("v");
        targets.push_back({.label = label, .value_type = type});
        auto const mark = scope.size();
        auto body = statements(depth - 1);
        body.push_back(b.leave(label, expr(type, depth - 1)));
        scope.resize_down_to(mark);
        targets.remove_back();
        return b.block_expr(label, type, body);
    }

    /// A read of a `var` to the left of a block that assigns it: the operand rule E2 must pin.
    flat_expr_id racy(type_id type, int depth)
    {
        auto candidates = cc::vector<local_id>();
        for (auto const& v : scope)
            if (v.type == type && v.is_assignable)
                candidates.push_back(v.local);
        if (candidates.empty())
            return block_expr(type, depth);

        // through a member where there is one: the pin has to see that `s.x` reads what `s.y = …` writes
        if (type == float_type && chance(40))
            if (auto const s = any_struct_var(); is_valid(s))
            {
                auto const read = b.member(b.local(s), pick(3));
                auto const label = b.add_label("m");
                auto body = cc::vector<flat_stmt_id>();
                body.push_back(b.assign(b.member(b.local(s), pick(3)), expr(type, 0)));
                body.push_back(b.leave(label, leaf(type)));
                auto const block = b.block_expr(label, type, body);
                return b.call("multiply", {read, block});
            }

        auto const var = candidates[pick(int(candidates.size()))];
        auto const read = b.local(var);
        auto const label = b.add_label("w");
        auto body = cc::vector<flat_stmt_id>();
        body.push_back(b.assign(b.local(var), expr(type, 0)));
        if (chance(50))
            body.push_back(b.print(b.local(var)));
        body.push_back(b.leave(label, leaf(type)));
        auto const block = b.block_expr(label, type, body);
        return b.call(type == float_type ? "add" : "add_int", {read, block});
    }

    flat_expr_id expr(type_id type, int depth)
    {
        --nodes_left;
        if (depth <= 0 || nodes_left <= 0)
            return leaf(type);

        auto const r = pick(10);
        if (type == float_type)
        {
            if (r <= 2)
                return leaf(type);
            if (r <= 5)
            {
                cc::string_view const operators[] = {"add", "subtract", "multiply"};
                auto const lhs = expr(type, depth - 1);
                auto const rhs = expr(type, depth - 1);
                return b.call(operators[r - 3], {lhs, rhs});
            }
            // now and then as a call with an effect, which the machine records: its place among the prints is observable
            if (r == 6)
                return chance(40) ? b.call_with_effect("saturate", {expr(type, depth - 1)})
                                  : b.call("saturate", {expr(type, depth - 1)});
            if (r == 9 && has_buffer_of(type) && chance(40))
                return racy_element(type);
            if (r == 8 && has_buffer_of(type) && chance(40))
                return buffer_read(type, depth);
            return r == 9 ? racy(type, depth) : block_expr(type, depth);
        }
        if (type == int_type)
        {
            if (r <= 3)
                return leaf(type);
            if (r <= 6)
            {
                cc::string_view const operators[] = {"add_int", "subtract_int", "multiply_int"};
                auto const lhs = expr(type, depth - 1);
                auto const rhs = expr(type, depth - 1);
                return b.call(operators[r - 4], {lhs, rhs});
            }
            return r == 9 ? racy(type, depth) : block_expr(type, depth);
        }

        if (r == 0)
            return leaf(type);
        if (r <= 2)
        {
            auto const lhs = expr(float_type, depth - 1);
            auto const rhs = expr(float_type, depth - 1);
            return b.call("less", {lhs, rhs});
        }
        if (r <= 4)
        {
            auto const lhs = expr(int_type, depth - 1);
            auto const rhs = expr(int_type, depth - 1);
            if (chance(25))
                return b.call_with_effect("less_int", {lhs, rhs});
            return b.call(r == 3 ? "less_int" : "equal_int", {lhs, rhs});
        }
        if (r == 5)
            return b.not_(expr(type, depth - 1));
        if (r == 8)
            return block_expr(type, depth);
        // the right side of 9 always has an effect, and the one of 6 and 7 has one when the dice say so
        auto const lhs = expr(type, depth - 1);
        auto const rhs = r == 9 ? block_expr(type, depth) : expr(type, depth - 1);
        return chance(50) ? b.and_(lhs, rhs) : b.or_(lhs, rhs);
    }

    // ---- statements -------------------------------------------------------------------------------------------------

    /// One list with a scope of its own.
    cc::vector<flat_stmt_id> statements(int depth)
    {
        auto const mark = scope.size();
        auto list = cc::vector<flat_stmt_id>();
        auto const count = rng.uniform(1, shape.max_statements);
        for (auto i = 0; i < count; ++i)
        {
            auto const is_exit = statement(depth, list);
            // what follows an exit is dead, which a legalizer must survive, so now and then it stays
            if (is_exit && !chance(25))
                break;
        }
        scope.resize_down_to(mark);
        return list;
    }

    /// True when the statement added last always exits.
    bool exit(int depth, cc::vector<flat_stmt_id>& list)
    {
        auto loops = cc::vector<label_id>();
        for (auto const& t : targets)
            if (t.is_loop)
                loops.push_back(t.label);

        auto stmt = flat_stmt_id::none;
        if (!loops.empty() && chance(35))
            stmt = b.continue_(loops[pick(int(loops.size()))]);
        else
        {
            auto const t = targets[pick(int(targets.size()))];
            stmt = is_valid(t.value_type) ? b.leave(t.label, expr(t.value_type, 1)) : b.leave(t.label);
        }

        if (!chance(75))
        {
            list.push_back(stmt);
            return true;
        }
        auto const condition = expr(bool_type, depth > 0 ? 1 : 0);
        auto then_body = cc::vector<flat_stmt_id>();
        if (chance(40))
            then_body.push_back(b.print(leaf(any_type())));
        then_body.push_back(stmt);
        list.push_back(b.if_(condition, then_body));
        return false;
    }

    void loop(int depth, cc::vector<flat_stmt_id>& list)
    {
        auto const limit = rng.uniform(1, 3);
        auto const label = b.add_label("l");
        auto const form = pick(3);

        if (form == 2)
        {
            auto const first = chance(20) ? block_expr(int_type, 1) : b.int_literal(0);
            auto end = b.int_literal(limit);
            auto const end_form = pick(3);
            if (end_form == 1)
            {
                auto const inner = b.add_label("e");
                end = b.block_expr(inner, int_type,
                                   {b.print(b.int_literal(limit)), b.leave(inner, b.int_literal(limit))});
            }
            else if (end_form == 2)
            {
                // a mutable end the body may assign: the bounds are evaluated once all the same
                auto const h = b.var("h", int_type, b.int_literal(limit));
                list.push_back(h.stmt);
                scope.push_back({.local = h.local, .type = int_type, .is_assignable = true});
                end = b.local(h.local);
            }
            auto const index = b.add_local(local_kind::index, "i", int_type);
            targets.push_back({.label = label, .is_loop = true});
            auto const mark = scope.size();
            scope.push_back({.local = index, .type = int_type});
            auto const body = statements(depth - 1);
            scope.resize_down_to(mark);
            targets.remove_back();
            list.push_back(b.for_(label, index, first, end, body));
            return;
        }

        // the counter is no assignable local, and it advances before anything in the body can `continue`
        auto const n = b.var("n", int_type, b.int_literal(0));
        list.push_back(n.stmt);
        scope.push_back({.local = n.local, .type = int_type});
        auto const in_range = [&] { return b.call("less_int", {b.local(n.local), b.int_literal(limit)}); };
        auto const advance
            = [&] { return b.assign(b.local(n.local), b.call("add_int", {b.local(n.local), b.int_literal(1)})); };

        if (form == 0)
        {
            targets.push_back({.label = label, .is_loop = true});
            auto body = cc::vector<flat_stmt_id>();
            body.push_back(b.if_(b.not_(in_range()), {b.leave(label)}));
            body.push_back(advance());
            body.push_back_range(statements(depth - 1));
            targets.remove_back();
            list.push_back(b.loop(label, body));
            return;
        }

        auto condition = in_range();
        if (chance(40))
        {
            auto const inner = b.add_label("c");
            condition = b.block_expr(inner, bool_type, {b.print(b.local(n.local)), b.leave(inner, condition)});
        }
        else if (chance(30))
            condition = b.and_(condition, expr(bool_type, 1));
        targets.push_back({.label = label, .is_loop = true});
        auto body = cc::vector<flat_stmt_id>();
        body.push_back(advance());
        body.push_back_range(statements(depth - 1));
        targets.remove_back();
        list.push_back(b.while_(label, condition, body));
    }

    /// True when the statement added last always exits.
    /// A `case` over an `int`, whose arms leave, continue and print like any other list.
    /// Its patterns are literals most of the time, which is the switch form, and leaves or blocks otherwise, which is
    /// the chain: a pattern that prints runs only where it is reached, so the chain has to keep it inside its branch.
    void generate_case(int depth, cc::vector<flat_stmt_id>& list)
    {
        auto const scrutinee = expr(int_type, 2);
        auto const wants_chain = chance(30);

        auto arms = cc::vector<flat_arm>();
        auto used = cc::vector<int>();
        auto const count = rng.uniform(1, 3);
        for (auto i = 0; i < count; ++i)
        {
            auto patterns = cc::vector<flat_expr_id>();
            auto const values = rng.uniform(1, 2);
            for (auto k = 0; k < values; ++k)
            {
                // A value twice would be two labels of one switch, which every target refuses.
                auto const value = pick(5);
                auto is_used = false;
                for (auto const u : used)
                    is_used = is_used || u == value;
                if (is_used)
                    continue;
                used.push_back(value);
                if (!wants_chain || chance(50))
                    patterns.push_back(b.int_literal(value));
                else if (chance(60))
                    patterns.push_back(leaf(int_type));
                else
                {
                    auto const label = b.add_label("p");
                    patterns.push_back(b.block_expr(
                        label, int_type, {b.print(b.int_literal(value)), b.leave(label, b.int_literal(value))}));
                }
            }
            if (patterns.empty())
                continue;
            auto const list_of_patterns = b.expr_list(patterns);
            auto const body = statements(depth - 1);
            arms.push_back({.patterns = list_of_patterns, .body = b.stmt_list(body)});
        }
        auto const default_body = statements(depth - 1);
        if (arms.empty())
        {
            list.push_back_range(default_body);
            return;
        }

        auto node
            = flat_case{.scrutinee = scrutinee, .arms = b.arm_list(arms), .default_body = b.stmt_list(default_body)};
        // The `==` the chain form calls; the builder resolves the overload, so a probe call hands over the record.
        auto const probe = b.call("equal_int", {b.int_literal(0), b.int_literal(0)});
        if (auto const* const c = b.e.at(probe).node.try_as<flat_call>())
        {
            node.equality = c->callee;
            node.equality_intrinsic = c->intrinsic;
        }
        list.push_back(b.add_stmt(cc::move(node)));
    }

    bool statement(int depth, cc::vector<flat_stmt_id>& list)
    {
        --nodes_left;
        auto const r = nodes_left <= 0 ? 0 : pick(100);
        auto const is_flat = depth <= 0;

        if (r < 25 || (is_flat && r >= 57 && r < 87))
        {
            // Now and then evaluated and dropped: what the value prints and records on its way still happens, in its place.
            auto const value = expr(any_type(), 2);
            list.push_back(chance(25) ? b.eval(value) : b.print(value));
            return false;
        }
        if (r < 35)
        {
            auto const declared = b.let("x", expr(any_type(), 2));
            list.push_back(declared.stmt);
            scope.push_back({.local = declared.local, .type = b.e.at(declared.local).type});
            return false;
        }
        if (r >= 35 && r < 38)
        {
            auto const x = expr(float_type, 1);
            auto const y = expr(float_type, 1);
            auto const z = expr(float_type, 1);
            auto const declared = b.var("s", float3_type, b.construct(float3_type, {x, y, z}));
            list.push_back(declared.stmt);
            scope.push_back({.local = declared.local, .type = float3_type});
            return false;
        }
        if (r >= 45 && r < 49)
            if (auto const s = any_struct_var(); is_valid(s))
            {
                list.push_back(b.assign(b.member(b.local(s), pick(3)), expr(float_type, 2)));
                return false;
            }
        if (r >= 38 && r < 42 && is_valid(store))
        {
            list.push_back(store_statement(2));
            return false;
        }
        if (r < 45)
        {
            auto const type = any_type();
            if (chance(15))
            {
                auto const declared = b.var("u", type);
                list.push_back(declared.stmt);
                list.push_back(b.assign(b.local(declared.local), leaf(type)));
                scope.push_back({.local = declared.local, .type = type, .is_assignable = true});
                return false;
            }
            auto const declared = b.var("y", type, expr(type, 2));
            list.push_back(declared.stmt);
            scope.push_back({.local = declared.local, .type = type, .is_assignable = true});
            return false;
        }
        if (r < 57)
        {
            auto candidates = cc::vector<visible>();
            for (auto const& v : scope)
                if (v.is_assignable)
                    candidates.push_back(v);
            if (candidates.empty())
            {
                list.push_back(b.print(expr(any_type(), 1)));
                return false;
            }
            auto const v = candidates[pick(int(candidates.size()))];
            list.push_back(b.assign(b.local(v.local), expr(v.type, 2)));
            return false;
        }
        if (r < 67)
        {
            auto const condition = expr(bool_type, 2);
            auto const then_body = statements(depth - 1);
            auto const else_body = chance(50) ? statements(depth - 1) : cc::vector<flat_stmt_id>();
            list.push_back(b.if_(condition, then_body, else_body));
            return false;
        }
        if (r < 71)
        {
            auto const label = b.add_label("b");
            targets.push_back({.label = label});
            auto const body = statements(depth - 1);
            targets.remove_back();
            list.push_back(b.block(label, body));
            return false;
        }
        if (r < 79)
        {
            generate_case(depth, list);
            return false;
        }
        if (r < 87)
        {
            loop(depth, list);
            return false;
        }
        return exit(depth, list);
    }
};
/// Whether every local is read and assigned only where a target would see its declaration.
/// The interpreter keeps one slot per local for the whole run, so it cannot tell; a target's compiler can.
struct scope_checker
{
    flat_entry_point const& e;
    cc::vector<local_id> declared;
    cc::string violation;

    void use(local_id id)
    {
        for (auto const d : declared)
            if (d == id)
                return;
        if (violation.empty())
            violation = cc::format("'{}' is used outside the list that declares it", e.at(id).name);
    }

    void expr(flat_expr_id id)
    {
        if (!is_valid(id))
            return;
        auto const& x = e.at(id);
        if (auto const* const ref = x.node.try_as<flat_local_ref>())
            use(ref->local);
        if (auto const* const block = x.node.try_as<flat_block>())
            body(block->body);
        impl::for_each_operand(e, x, [&](flat_expr_id operand) { expr(operand); });
    }

    void body(sgl::ast::range_of<flat_stmt_id> range)
    {
        auto const mark = declared.size();
        for (auto const id : e.at(range))
        {
            auto const& s = e.at(id);
            impl::for_each_expr_of(s, [&](flat_expr_id x) { expr(x); });
            if (auto const* const let = s.node.try_as<flat_let>())
                declared.push_back(let->local);
            if (auto const* const var = s.node.try_as<flat_var>())
                declared.push_back(var->local);
            if (auto const* const loop = s.node.try_as<flat_for>())
                declared.push_back(loop->index);
            impl::for_each_body_of(e, s, [&](sgl::ast::range_of<flat_stmt_id> inner) { body(inner); });
            // the index of a `for` is gone behind its loop
            if (s.node.is<flat_for>())
                declared.remove_back();
        }
        declared.resize_down_to(mark);
    }
};

cc::string scope_violation(flat_entry_point const& e)
{
    auto checker = scope_checker{.e = e};
    checker.declared.push_back(local_id(0));
    checker.body(e.body);
    return cc::move(checker.violation);
}
} // namespace

flat_entry_point sgl_test::random_program(checked_module const& m, u64 seed, program_shape const& shape)
{
    auto g = generator{.b = float_function(m), .rng = cc::random(seed), .shape = shape, .nodes_left = shape.max_nodes};
    g.float_type = g.b.type_named("float");
    g.int_type = g.b.type_named("int");
    g.bool_type = g.b.type_named("bool");
    g.float3_type = g.b.type_named("float3");
    g.store = store_binding(m);
    if (is_valid(g.store))
        g.b.e.bindings.push_back(g.store);
    g.targets.push_back({.label = g.b.e.root, .value_type = g.float_type});

    auto body = g.statements(shape.max_depth);
    // generous again, so that the value the function ends in is as rich as the statements before it
    g.nodes_left = cc::max(g.nodes_left, 12);
    body.push_back(g.b.leave(g.b.e.root, g.expr(g.float_type, 2)));
    g.b.set_body(body);
    return cc::move(g.b.e);
}

cc::string sgl_test::differential_failure(checked_module const& m,
                                          u64 seed,
                                          program_shape const& shape,
                                          legalize_options const& options)
{
    auto const structured = random_program(m, seed, shape);
    auto const inputs = test_inputs(m);
    auto const limits = run_limits{.fuel = 400'000};
    auto const expected = interpret(m, structured, inputs, limits);

    auto const report = [&](cc::string_view what, flat_entry_point const* core, outcome const* actual)
    {
        auto text = cc::format("seed {} (depth {}, statements {}, nodes {}): {}\n", seed, shape.max_depth,
                               shape.max_statements, shape.max_nodes, what);
        text.appendf("structured: {}\n{}", dump(expected), dump_entry_point(m, structured));
        if (core != nullptr)
            text.appendf("core: {}\n{}", actual != nullptr ? dump(*actual) : cc::string(), dump_entry_point(m, *core));
        return text;
    };

    // a program the generator got wrong proves nothing about the legalizer
    if (expected.status != run_status::ok)
        return report("the generated program does not run to a result", nullptr, nullptr);
    if (auto const violation = scope_violation(structured); !violation.empty())
        return report(cc::format("the generated program is not scoped: {}", violation), nullptr, nullptr);

    auto const core = legalize(m, structured, options);
    if (auto const violation = find_core_violation(core); violation.has_value())
        return report(cc::format("the legalized tree is not core: {}", violation.value().reason), &core, nullptr);

    if (auto const violation = scope_violation(core); !violation.empty())
        return report(cc::format("the legalized tree is not scoped: {}", violation), &core, nullptr);

    auto const actual = interpret(m, core, inputs, limits);
    if (!(actual == expected))
        return report("the two forms behave differently", &core, &actual);
    return "";
}
