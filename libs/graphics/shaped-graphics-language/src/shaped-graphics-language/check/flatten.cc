#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// A call reached from an entry point of a stage its callee's `@stages` leaves out (CHK-193).
struct stage_violation
{
    i32 file = 0;
    ast::expr_id call = ast::expr_id::none;
    symbol_id callee = symbol_id::none;
};

/// Reads the checked ASTs through the side tables and writes the STRUCTURED form of one entry point's flat tree.
///
/// A call of a function of the program is inlined where it stands: a block named after the callee, whose `return`s are
/// leaves of that block.
/// Nothing here hoists and nothing reorders; evaluation order is the legalizer's job alone.
/// Every body is known to be sound, so an unexpected shape is a gap of this pass: it sets `is_failed`, and the entry
/// point is dropped with an `unsupported-yet` at its name (CHK-213), which keeps a half-built tree away from every
/// emitter.
/// A value of the error type is not a gap: whatever gave it that type reported already.
struct flattener
{
    checker const& c;
    cc::vector<stage_violation> stage_violations;

    /// Notes a call of `callee` whose `@stages` leaves out the stage of the entry point being flattened.
    void judge_stage(ast::expr_id call, symbol_id callee)
    {
        auto const& s = c.out.at(callee);
        if (s.info >= 0 && (c.out.functions[s.info].stages & stage_bit(entry.entry_stage)) == 0)
            stage_violations.push_back({.file = file(), .call = call, .callee = callee});
    }
    flat_entry_point entry;
    bool is_failed = false;
    /// The tree met the error type somewhere, so a failure has a diagnostic already (CHK-7).
    bool meets_error = false;

    /// True where the error type stands in `type` at any depth.
    [[nodiscard]] bool holds_error(type_id type) const
    {
        if (type == checked_module::error_type)
            return true;
        if (!is_valid(type))
            return false;
        for (auto const& m : c.out.at(c.out.at(type).members))
            if (holds_error(m.type))
                return true;
        return false;
    }

    /// What a name of the source stands for, keyed the way `target_of` names it.
    struct bound_name
    {
        target where;
        local_id local = local_id::none;
        /// A parameter whose argument is a literal: the literal stands wherever the parameter is read.
        flat_expr_id literal = flat_expr_id::none;
    };

    /// A loop of the source around the statement being written.
    struct loop_target
    {
        /// What a `continue` names, and a `break` without a value.
        label_id loop = label_id::none;
        /// The block around a `loop:` that is a value, which a `break value` leaves.
        label_id value_block = label_id::none;
    };

    /// One function being written: the entry point at the bottom, then every call being inlined.
    struct frame
    {
        symbol_id function = symbol_id::none;
        i32 file = 0;
        type_id result = type_id::none;
        /// The block a `return` leaves; `none` for the entry point, whose `return` is the flat tree's.
        label_id return_label = label_id::none;
        /// The call sites every node of this frame was inlined through.
        ast::range_of<call_site> chain;
        cc::vector<bound_name> bound;
        cc::vector<loop_target> loops;
        /// The value blocks of the `case` arms being written, innermost last; a `yield` leaves the last.
        cc::vector<label_id> value_blocks;
    };
    cc::vector<frame> frames;

    /// The innermost frame, as a handle rather than a reference.
    ///
    /// Inlining a call pushes a frame, which can move the stack, so a reference into it goes stale the moment
    /// anything is flattened through it — and a stale read here decides whether a `return` leaves the callee's
    /// block or the entry point, which is a miscompilation and not a crash.
    /// Outside a release build this checks that the stack has not moved since the handle was made.
    struct frame_ref
    {
        cc::vector<frame>* owner = nullptr;
#if CC_ASSERT_ENABLED
        frame const* base = nullptr;
#endif
        isize at = 0;

        [[nodiscard]] frame* operator->() const
        {
#if CC_ASSERT_ENABLED
            CC_ASSERT(owner->data() == base, "a frame handle outlived a push: inlining a call moves the stack, so read "
                                             "what you need first");
#endif
            return owner->data() + at;
        }
        [[nodiscard]] frame& operator*() const { return *operator->(); }
    };

    [[nodiscard]] frame_ref current()
    {
        auto ref = frame_ref{.owner = &frames, .at = frames.size() - 1};
#if CC_ASSERT_ENABLED
        ref.base = frames.data();
#endif
        return ref;
    }

    /// The statements of the list being written.
    cc::vector<flat_stmt_id> block;

    /// Read inside ONE expression, which no push can come between; everything else goes through `current`.
    [[nodiscard]] i32 file() const { return frames.back().file; }
    [[nodiscard]] ast::file_ast const& ast() const { return c.ast_of(file()); }
    [[nodiscard]] file_tables const& tables() const { return c.out.files[file()]; }

    flat_expr_id fail()
    {
        is_failed = true;
        return flat_expr_id::none;
    }

    local_id add_local(local_kind kind, cc::string_view desired, type_id type)
    {
        is_failed = is_failed || !c.is_sound(type);
        meets_error = meets_error || holds_error(type);
        // `_` is a name nobody reads, and no name at all in WGSL
        entry.locals.push_back({.kind = kind,
                                .name = entry.names.mint(desired == "_" ? cc::string_view("unused") : desired),
                                .type = type,
                                .is_mut = kind == local_kind::var});
        return local_id(entry.locals.size() - 1);
    }

    label_id add_label(cc::string_view desired)
    {
        auto name = cc::string(desired);
        auto const is_taken = [&]
        {
            for (auto const& l : entry.labels)
                if (l.name == name)
                    return true;
            return false;
        };
        for (auto i = 1; is_taken(); ++i)
            name = cc::format("{}_{}", desired, i);
        entry.labels.push_back({.name = cc::move(name)});
        return label_id(entry.labels.size() - 1);
    }

    template <class Node>
    flat_expr_id add_expr(type_id type, ast::expr_id from, Node node)
    {
        // A builtin with an effect may give nothing, `DEBUG_store`, and its call is only ever an `eval`'s value.
        auto const is_effect_call = std::is_same_v<Node, flat_call> && type == checked_module::nothing_type;
        is_failed = is_failed || (!c.is_sound(type) && !is_effect_call);
        entry.exprs.push_back({.type = type,
                               .from = {.file = file(), .expr = from},
                               .inlined_through = current()->chain,
                               .node = cc::move(node)});
        return flat_expr_id(entry.exprs.size() - 1);
    }

    template <class Node>
    flat_stmt_id make_stmt(origin from, Node node)
    {
        entry.stmts.push_back({.from = from, .inlined_through = current()->chain, .node = cc::move(node)});
        return flat_stmt_id(entry.stmts.size() - 1);
    }

    template <class Node>
    void add_stmt(origin from, Node node)
    {
        block.push_back(make_stmt(from, cc::move(node)));
    }

    ast::range_of<flat_expr_id> add_list(cc::span<flat_expr_id const> list)
    {
        auto const range = ast::range_of<flat_expr_id>{.first = u32(entry.expr_lists.size()), .count = u32(list.size())};
        entry.expr_lists.push_back_range(list);
        return range;
    }

    ast::range_of<flat_stmt_id> add_list(cc::span<flat_stmt_id const> list)
    {
        auto const range = ast::range_of<flat_stmt_id>{.first = u32(entry.stmt_lists.size()), .count = u32(list.size())};
        entry.stmt_lists.push_back_range(list);
        return range;
    }

    ast::range_of<flat_arm> add_arms(cc::span<flat_arm const> list)
    {
        auto const range = ast::range_of<flat_arm>{.first = u32(entry.arms.size()), .count = u32(list.size())};
        entry.arms.push_back_range(list);
        return range;
    }

    flat_expr_id local_ref(local_id local, ast::expr_id from)
    {
        return add_expr(entry.at(local).type, from, flat_local_ref{.local = local});
    }

    /// True for what may stand in several places and be evaluated in any of them: a literal, or an immutable local.
    [[nodiscard]] bool is_substitutable(flat_expr_id id) const
    {
        if (!is_valid(id))
            return false;
        auto const& x = entry.at(id);
        if (x.node.is<flat_literal>() || x.node.is<flat_int_literal>() || x.node.is<flat_bool_literal>()
            || x.node.is<flat_enum_value>())
            return true;
        auto const* const ref = x.node.try_as<flat_local_ref>();
        return ref != nullptr && !entry.at(ref->local).is_mut;
    }

    /// One more node that means what the substitutable `id` means, attributed to `from`.
    flat_expr_id again(flat_expr_id id, ast::expr_id from)
    {
        auto const x = entry.at(id);
        if (auto const* const ref = x.node.try_as<flat_local_ref>())
            return local_ref(ref->local, from);
        if (auto const* const l = x.node.try_as<flat_literal>())
            return add_expr(x.type, from, *l);
        if (auto const* const l = x.node.try_as<flat_int_literal>())
            return add_expr(x.type, from, *l);
        if (auto const* const l = x.node.try_as<flat_bool_literal>())
            return add_expr(x.type, from, *l);
        if (auto const* const v = x.node.try_as<flat_enum_value>())
            return add_expr(x.type, from, *v);
        return fail();
    }

    /// A value that is read more than once and evaluated once, where it stands.
    /// `first` is what stands in place of the value, and every later read is `again(later, …)`.
    struct evaluated_once
    {
        flat_expr_id first = flat_expr_id::none;
        flat_expr_id later = flat_expr_id::none;
    };

    /// A substitutable value stays as it is.
    /// Any other becomes `block $name { let name = value; leave $name name }`, and the later reads name that local:
    /// the block stands where the value stood, so nothing is evaluated earlier or later than the source says.
    evaluated_once evaluate_once(flat_expr_id value, cc::string_view name, ast::expr_id from)
    {
        if (!is_valid(value) || is_substitutable(value))
            return {.first = value, .later = value};

        auto const type = entry.at(value).type;
        auto const local = add_local(local_kind::temporary, name, type);
        auto const label = add_label(name);
        auto const where = origin{.file = file(), .expr = from};
        flat_stmt_id const body[] = {
            make_stmt(where, flat_let{.local = local, .value = value}),
            make_stmt(where, flat_leave{.target = label, .value = local_ref(local, from)}),
        };
        return {.first = add_expr(type, from, flat_block{.label = label, .body = add_list(body)}),
                .later = local_ref(local, from)};
    }

    // ---- expressions ------------------------------------------------------------------------------------------------

    flat_expr_id flatten_expr(ast::expr_id id)
    {
        if (!ast::is_valid(id))
            return fail();
        auto const& e = ast().at(id);
        auto const type = tables().type_at(id);
        auto const& where = tables().target_at(id);
        meets_error = meets_error || holds_error(type);
        if (!is_valid(type))
            return fail();

        if (e.node.is<ast::literal>())
            return flatten_number(id, type);
        if (e.node.is<ast::leading_dot>())
        {
            return where.kind == target_kind::enum_case ? add_expr(type, id, flat_enum_value{.case_index = where.index})
                                                        : fail();
        }
        if (e.node.is<ast::name>())
        {
            for (auto const& b : current()->bound)
                if (b.where == where)
                    return is_valid(b.literal) ? again(b.literal, id) : local_ref(b.local, id);
            return fail();
        }
        if (auto const* const m = e.node.try_as<ast::member>())
        {
            if (where.kind == target_kind::enum_case)
                return add_expr(type, id, flat_enum_value{.case_index = where.index});
            if (where.kind == target_kind::binding_member)
                return add_expr(type, id, flat_binding_member{.binding = where.symbol, .member = where.index});
            if (where.kind != target_kind::field)
                return fail();
            auto const object = flatten_expr(m->object);
            return add_expr(type, id, flat_member{.object = object, .member = where.index});
        }
        if (auto const* const indexed = e.node.try_as<ast::index>())
        {
            // The check pass let only a buffer element through, so the object is a resource and the index an int.
            auto const arguments = ast().at(indexed->arguments);
            if (arguments.size() != 1)
                return fail();
            auto const buffer = flatten_expr(indexed->object);
            auto const index = flatten_expr(arguments[0].value);
            return add_expr(type, id, flat_buffer_element{.buffer = buffer, .index = index});
        }
        if (auto const* const call = e.node.try_as<ast::call>())
            return flatten_call(id, type, where, *call);
        if (auto const* const cast = e.node.try_as<ast::cast>())
        {
            auto const value = flatten_expr(cast->value);
            // A cast to the type the value already has is the value itself.
            if (where.kind != target_kind::overload)
                return value;
            flat_expr_id const arguments[] = {value};
            // A conversion the program declares is inlined like any call of it (CHK-195).
            if (!is_valid(c.out.at(where.symbol).intrinsic))
            {
                auto const inlined = inline_call(id, where.symbol, arguments);
                return add_expr(type, id, flat_block{.label = inlined.label, .body = inlined.body});
            }
            return builtin_call(id, where.symbol, arguments);
        }
        if (auto const* const chain = e.node.try_as<ast::comparison_chain>())
            return flatten_chain(id, type, *chain);
        if (auto const* const loop = e.node.try_as<ast::loop_expr>())
            return flatten_value_loop(id, type, *loop);
        if (auto const* const c = e.node.try_as<ast::case_expr>())
            return flatten_value_case(id, type, *c);
        return fail();
    }

    flat_expr_id flatten_number(ast::expr_id id, type_id type)
    {
        auto const text = c.text_of(file(), c.span_of(file(), id));
        if (classify_number(text) == number_class::plain_integer)
        {
            auto const value = parse_plain_integer(text);
            return value.has_value() ? add_expr(type, id, flat_int_literal{.value = value.value()}) : fail();
        }
        auto const value = parse_plain_float(text);
        return value.has_value() ? add_expr(type, id, flat_literal{.value = value.value()}) : fail();
    }

    flat_expr_id flatten_call(ast::expr_id id, type_id type, target const& where, ast::call const& call)
    {
        if (sgl::is_valid(call.op))
        {
            auto const spelling = c.text_of(file(), c.file_of(file()).at(call.op).where);
            auto const arguments = ast().at(call.arguments);
            if (spelling == "not" && arguments.size() == 1)
                return add_expr(type, id, flat_not{.operand = flatten_expr(arguments[0].value)});
            if (call.is_short_circuit && arguments.size() == 2)
            {
                auto const lhs = flatten_expr(arguments[0].value);
                auto const rhs = flatten_expr(arguments[1].value);
                return spelling == "and" ? add_expr(type, id, flat_and{.lhs = lhs, .rhs = rhs})
                                         : add_expr(type, id, flat_or{.lhs = lhs, .rhs = rhs});
            }
            if (spelling == "not" || call.is_short_circuit)
                return fail();
        }

        auto const arguments = flatten_arguments(call.arguments);
        if (where.kind == target_kind::constructor)
            return add_expr(type, id, flat_construct{.arguments = add_list(arguments)});
        if (where.kind != target_kind::overload)
            return fail();
        if (!is_valid(c.out.at(where.symbol).intrinsic))
        {
            auto const inlined = inline_call(id, where.symbol, arguments);
            return add_expr(type, id, flat_block{.label = inlined.label, .body = inlined.body});
        }
        return builtin_call(id, where.symbol, arguments);
    }

    flat_expr_id builtin_call(ast::expr_id id, symbol_id callee, cc::span<flat_expr_id const> arguments)
    {
        judge_stage(id, callee);
        auto const& s = c.out.at(callee);
        if (!is_valid(s.intrinsic) || s.info < 0)
            return fail();
        auto const& info = c.out.functions[s.info];
        return add_expr(info.result, id,
                        flat_call{.callee = callee,
                                  .intrinsic = s.intrinsic,
                                  .is_pure = info.is_pure,
                                  .arguments = add_list(arguments)});
    }

    cc::vector<flat_expr_id> flatten_arguments(ast::range_of<ast::argument> range)
    {
        auto result = cc::vector<flat_expr_id>();
        for (auto const& a : ast().at(range))
        {
            if (!a.is_splat)
            {
                result.push_back(flatten_expr(a.value));
                continue;
            }

            // A splat stands for one member access per field, and its value is evaluated once: where the first one stands.
            auto const value = flatten_expr(a.value);
            if (!is_valid(value))
                continue;
            auto const is_local = entry.at(value).node.is<flat_local_ref>();
            auto const once
                = is_local ? evaluated_once{.first = value, .later = value} : evaluate_once(value, "splat", a.value);
            auto const members = c.out.at(c.out.at(entry.at(value).type).members);
            for (auto i = isize(0); i < members.size(); ++i)
            {
                auto const object = i == 0 ? once.first : again(once.later, a.value);
                result.push_back(add_expr(members[i].type, a.value, flat_member{.object = object, .member = i32(i)}));
            }
        }
        return result;
    }

    /// `a < b <= c` is `a < b and b <= c` with `b` evaluated once, and the `and` keeps `c` unevaluated where it must.
    flat_expr_id flatten_chain(ast::expr_id id, type_id type, ast::comparison_chain const& chain)
    {
        // copies: the AST is stable, and the spans are not needed past the loop
        auto const operands = ast().at(chain.operands);
        auto const operators = ast().at(chain.operators);
        if (operands.size() < 2 || operators.size() + 1 != operands.size())
            return fail();

        auto comparisons = cc::vector<flat_expr_id>();
        auto left = evaluated_once{.first = flatten_expr(operands[0])};
        for (auto i = isize(0); i < operators.size(); ++i)
        {
            auto const is_last = i + 1 == operators.size();
            auto const value = flatten_expr(operands[i + 1]);
            auto const right
                = is_last ? evaluated_once{.first = value} : evaluate_once(value, "chained", operands[i + 1]);
            if (!is_valid(left.first) || !is_valid(right.first))
                return fail();

            type_id const types[] = {entry.at(left.first).type, entry.at(right.first).type};
            auto const spelling = c.text_of(file(), c.file_of(file()).at(operators[i]).where);
            auto const callee = c.find_operator(spelling, types);
            if (!is_valid(callee))
                return fail();
            flat_expr_id const arguments[] = {left.first, right.first};
            comparisons.push_back(builtin_call(id, callee, arguments));
            if (!is_last)
                left = {.first = again(right.later, operands[i + 1])};
        }

        // right-nested, so each comparison runs only when every one before it held
        auto result = comparisons.back();
        for (auto i = comparisons.size() - 2; i >= 0; --i)
            result = add_expr(type, id, flat_and{.lhs = comparisons[i], .rhs = result});
        return result;
    }

    /// A `loop:` somebody reads the value of: a block around the loop, which a `break value` leaves.
    flat_expr_id flatten_value_loop(ast::expr_id id, type_id type, ast::loop_expr const& loop)
    {
        auto const value_block = add_label("loop_value");
        auto const label = add_label("loop");
        current()->loops.push_back({.loop = label, .value_block = value_block});
        auto const body = flatten_body(loop.body);
        current()->loops.remove_back();

        auto const where = origin{.file = file(), .expr = id};
        flat_stmt_id const statements[] = {make_stmt(where, flat_loop{.label = label, .body = body})};
        return add_expr(type, id, flat_block{.label = value_block, .body = add_list(statements)});
    }

    /// The scrutinee, the arms and the default of a `case`; `value_block` is the block an arm's value leaves.
    flat_case flatten_case_parts(ast::case_expr const& node, label_id value_block)
    {
        auto const scrutinee = flatten_expr(node.value);
        auto result = flat_case{.scrutinee = scrutinee, .equality = equality_for(scrutinee)};
        if (is_valid(result.equality))
            result.equality_intrinsic = c.out.at(result.equality).intrinsic;
        auto arms = cc::vector<flat_arm>();
        auto has_default = false;

        for (auto const& arm : ast().at(node.arms))
        {
            auto const is_wildcard = ast::is_valid(arm.pattern) && ast().at(arm.pattern).node.is<ast::wildcard>();
            auto patterns = cc::vector<flat_expr_id>();
            if (!is_wildcard)
                collect_patterns(arm.pattern, patterns);
            auto const body = flatten_arm_body(arm, value_block);
            if (is_wildcard)
            {
                result.default_body = body;
                has_default = true;
            }
            else
                arms.push_back({.patterns = add_list(patterns), .body = body});
        }

        // Every tree has a default: without a `_` the source was exhaustive by its cases, and the last arm is it.
        if (!has_default && !arms.empty())
        {
            result.default_body = arms.back().body;
            arms.remove_back();
        }
        result.arms = add_arms(arms);
        return result;
    }

    /// The `==` that compares the scrutinee with a pattern; an enum compares as the `int` its cases are.
    symbol_id equality_for(flat_expr_id scrutinee) const
    {
        if (!is_valid(scrutinee))
            return symbol_id::none;
        auto const type = entry.at(scrutinee).type;
        if (!is_valid(type))
            return symbol_id::none;
        // An enum compares as the `int` its cases are (EVAL-64).
        auto const compared = c.out.at(type).kind == type_kind::enumeration ? int_type() : type;
        if (!is_valid(compared))
            return symbol_id::none;
        type_id const both[] = {compared, compared};
        return c.find_operator("==", both);
    }

    /// The prelude's `int`, which an enum's comparison runs on.
    type_id int_type() const
    {
        for (auto const& s : c.out.symbols)
            if (s.name == builtins::k_int && s.kind == symbol_kind::structure)
                return s.type;
        return type_id::none;
    }

    /// `a or b` is a list of patterns rather than the `or` of the language (CHK-157).
    void collect_patterns(ast::expr_id id, cc::vector<flat_expr_id>& into)
    {
        if (!ast::is_valid(id))
            return;
        auto const& e = ast().at(id);
        if (auto const* const call = e.node.try_as<ast::call>();
            call != nullptr && call->is_short_circuit && sgl::is_valid(call->op)
            && c.text_of(file(), c.file_of(file()).at(call->op).where) == "or")
        {
            for (auto const& argument : ast().at(call->arguments))
                collect_patterns(argument.value, into);
            return;
        }
        into.push_back(flatten_expr(id));
    }

    ast::range_of<flat_stmt_id> flatten_arm_body(ast::case_arm const& arm, label_id value_block)
    {
        auto const outer = cc::move(block);
        block = {};
        if (is_valid(value_block))
            current()->value_blocks.push_back(value_block);

        if (arm.result.kind == ast::body_kind::arrow && ast::is_valid(arm.result.value))
        {
            auto const where = origin{.file = file(), .expr = arm.result.value};
            auto const& e = ast().at(arm.result.value);
            auto const is_jump = e.node.is<ast::return_expr>() || e.node.is<ast::break_expr>()
                              || e.node.is<ast::continue_expr>() || e.node.is<ast::yield_expr>();
            if (is_jump)
                flatten_expr_stmt(where, arm.result.value);
            else if (is_valid(value_block))
            {
                auto const value = flatten_expr(arm.result.value);
                add_stmt(where, flat_leave{.target = value_block, .value = value});
            }
            else
                flatten_expr_stmt(where, arm.result.value);
        }
        else
            for (auto const stmt : ast().at(arm.result.statements))
                flatten_stmt(stmt);

        if (is_valid(value_block))
            current()->value_blocks.remove_back();
        auto const range = add_list(block);
        block = cc::move(outer);
        return range;
    }

    /// A `case` somebody reads the value of: a block around it, which every arm leaves.
    flat_expr_id flatten_value_case(ast::expr_id id, type_id type, ast::case_expr const& node)
    {
        auto const value_block = add_label("case");
        auto const parts = flatten_case_parts(node, value_block);
        auto const where = origin{.file = file(), .expr = id};
        flat_stmt_id const statements[] = {make_stmt(where, parts)};
        return add_expr(type, id, flat_block{.label = value_block, .body = add_list(statements)});
    }

    /// An object that converts to `type`: one value per field, in FIELD order, whatever order the source names them in.
    flat_expr_id flatten_object(ast::expr_id id, type_id type)
    {
        auto const elements = ast().at(ast().at(id).node.as<ast::object>().elements);
        auto values = cc::vector<flat_expr_id>();
        for (auto const& m : c.out.at(c.out.at(type).members))
        {
            ast::argument const* named = nullptr;
            for (auto const& e : elements)
                if (c.text_of(file(), e.name) == m.name)
                    named = &e;
            if (named == nullptr)
                return fail();
            auto const is_object = ast::is_valid(named->value) && ast().at(named->value).node.is<ast::object>();
            values.push_back(is_object ? flatten_object(named->value, m.type) : flatten_expr(named->value));
        }
        return add_expr(type, id, flat_construct{.arguments = add_list(values)});
    }

    // ---- calls ------------------------------------------------------------------------------------------------------

    struct inlined_body
    {
        label_id label = label_id::none;
        ast::range_of<flat_stmt_id> body;
    };

    /// The body of `callee` as the statements of a block named after it.
    /// `arguments` were written in the caller's frame, left to right, and are bound at the top of the block in that order.
    inlined_body inline_call(ast::expr_id call, symbol_id callee, cc::span<flat_expr_id const> arguments)
    {
        judge_stage(call, callee);
        auto const& s = c.out.at(callee);
        auto is_open = s.info < 0 || frames.size() > k_max_inline_depth;
        for (auto const& f : frames)
            is_open = is_open || f.function == callee;
        auto const* const decl = is_open ? nullptr : c.ast_of(s.file).at(s.declaration).node.try_as<ast::fun_decl>();
        // recursion was reported by the check pass, and an entry point that reaches it is never written
        if (decl == nullptr)
        {
            is_failed = true;
            return {};
        }
        auto const& info = c.out.functions[s.info];
        auto const parameters = c.out.at(info.parameters);
        if (parameters.size() != arguments.size())
        {
            is_failed = true;
            return {};
        }

        // The chain is copied first, since `call_sites` grows under the span that names it.
        auto chain = cc::vector<call_site>();
        chain.push_back_range(entry.at(current()->chain));
        chain.push_back({.file = file(), .call = call});
        auto const chain_range
            = ast::range_of<call_site>{.first = u32(entry.call_sites.size()), .count = u32(chain.size())};
        entry.call_sites.push_back_range(chain);

        auto const label = add_label(s.name);
        auto const outer = cc::move(block);
        block = {};

        // A parameter is a value: a literal or an immutable local stands for it, and anything else is bound once.
        auto bound = cc::vector<bound_name>();
        for (auto i = isize(0); i < parameters.size(); ++i)
        {
            auto const where = target{.kind = target_kind::parameter, .index = i32(parameters[i].field)};
            auto const argument = arguments[i];
            if (!is_valid(argument))
            {
                is_failed = true;
                continue;
            }
            auto const& x = entry.at(argument);
            auto const* const ref = x.node.try_as<flat_local_ref>();
            if (ref != nullptr && is_substitutable(argument))
                bound.push_back({.where = where, .local = ref->local});
            else if (is_substitutable(argument))
                bound.push_back({.where = where, .literal = argument});
            else
            {
                auto const local = add_local(local_kind::let, parameters[i].name, x.type);
                add_stmt(x.from, flat_let{.local = local, .value = argument});
                bound.push_back({.where = where, .local = local});
            }
        }

        frames.push_back({.function = callee,
                          .file = s.file,
                          .result = info.result,
                          .return_label = label,
                          .chain = chain_range,
                          .bound = cc::move(bound)});
        if (ast::is_valid(decl->body.value))
            flatten_return({.file = s.file, .expr = decl->body.value}, decl->body.value);
        for (auto const stmt : ast().at(decl->body.statements))
            flatten_stmt(stmt);
        frames.remove_back();

        auto const body = add_list(block);
        block = cc::move(outer);
        return {.label = label, .body = body};
    }

    // ---- statements -------------------------------------------------------------------------------------------------

    ast::range_of<flat_stmt_id> flatten_body(ast::body const& body)
    {
        auto const outer = cc::move(block);
        block = {};
        for (auto const stmt : ast().at(body.statements))
            flatten_stmt(stmt);
        auto const range = add_list(block);
        block = cc::move(outer);
        return range;
    }

    void flatten_return(origin from, ast::expr_id value)
    {
        // by value: flattening the value may inline a call, whose frame moves the vector this frame lives in
        auto const return_label = current()->return_label;
        auto const result_type = current()->result;
        auto result = flat_expr_id::none;
        if (ast::is_valid(value))
        {
            auto const is_object = ast().at(value).node.is<ast::object>();
            result = is_object ? flatten_object(value, result_type) : flatten_expr(value);
        }
        if (is_valid(return_label))
            add_stmt(from, flat_leave{.target = return_label, .value = result});
        else
            add_stmt(from, flat_return{.value = result});
    }

    void flatten_if(origin from, cc::span<ast::if_branch const> branches)
    {
        if (branches.empty())
            return;
        auto const& first = branches.front();
        // a stray `else` was reported by the AST pass, and no sound body holds one
        if (!ast::is_valid(first.condition))
        {
            is_failed = true;
            return;
        }
        auto const condition = flatten_expr(first.condition);
        auto const then_body = flatten_body(first.then);

        auto else_body = ast::range_of<flat_stmt_id>();
        auto const rest = branches.subspan({.offset = 1, .size = branches.size() - 1});
        if (!rest.empty() && !ast::is_valid(rest.front().condition))
            else_body = flatten_body(rest.front().then);
        else if (!rest.empty())
        {
            auto const outer = cc::move(block);
            block = {};
            flatten_if(from, rest);
            else_body = add_list(block);
            block = cc::move(outer);
        }
        add_stmt(from, flat_if{.condition = condition, .then_body = then_body, .else_body = else_body});
    }

    void flatten_assign(origin from, ast::assign_stmt const& assign)
    {
        auto const place = flatten_expr(assign.target);
        auto value = flatten_expr(assign.value);
        auto const op = sgl::is_valid(assign.op) ? c.text_of(file(), c.file_of(file()).at(assign.op).where) : "";
        if (!is_valid(place) || !is_valid(value) || op.empty())
        {
            is_failed = true;
            return;
        }
        if (op != "=")
        {
            type_id const types[] = {entry.at(place).type, entry.at(value).type};
            auto const callee = c.find_operator(op.subview({.offset = 0, .size = op.size() - 1}), types);
            if (!is_valid(callee))
            {
                is_failed = true;
                return;
            }
            flat_expr_id const arguments[] = {read_of_place(place, assign.target), value};
            value = builtin_call(assign.value, callee, arguments);
        }
        add_stmt(from, flat_assign{.place = place, .value = value});
    }

    /// What `place op= value` reads the place as, which is the place read a second time.
    /// A local or a member of one holds nothing to evaluate, so it is simply flattened again.
    /// A buffer element's index is evaluated once (EVAL-14): the place gets its first evaluation, and the read the local
    /// that holds it.
    flat_expr_id read_of_place(flat_expr_id place, ast::expr_id target)
    {
        if (!is_valid(place) || !entry.at(place).node.is<flat_buffer_element>())
            return flatten_expr(target);

        // by value: evaluating once adds nodes, and the arrays move
        auto const x = entry.at(place);
        auto const element = x.node.as<flat_buffer_element>();
        auto const& indexed = ast().at(target).node.as<ast::index>();
        auto const arguments = ast().at(indexed.arguments);
        if (arguments.size() != 1)
            return fail();

        auto const once = evaluate_once(element.index, "index", arguments[0].value);
        entry.exprs[index_of(place)].node = flat_buffer_element{.buffer = element.buffer, .index = once.first};
        auto const buffer = entry.at(element.buffer);
        auto const buffer_again = add_expr(buffer.type, indexed.object, buffer.node);
        auto const index_again = once.later == once.first ? again(once.first, arguments[0].value) : once.later;
        return add_expr(x.type, target, flat_buffer_element{.buffer = buffer_again, .index = index_again});
    }

    void flatten_for(origin from, ast::stmt_id id, ast::for_stmt const& loop)
    {
        auto const* const r = ast::is_valid(loop.iterable) ? ast().at(loop.iterable).node.try_as<ast::range>() : nullptr;
        if (r == nullptr)
        {
            is_failed = true;
            return;
        }
        auto const first = flatten_expr(r->first);
        auto const end = flatten_expr(r->last);
        auto const* const n = ast::is_valid(loop.variable) ? ast().at(loop.variable).node.try_as<ast::name>() : nullptr;
        auto const int_type = is_valid(first) ? entry.at(first).type : type_id::none;
        auto const index
            = add_local(local_kind::index, n != nullptr ? c.text_of(file(), n->where) : cc::string_view("i"), int_type);
        current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)}, .local = index});

        auto const label = add_label("for");
        current()->loops.push_back({.loop = label});
        auto const body = flatten_body(loop.body);
        current()->loops.remove_back();
        add_stmt(from, flat_for{.label = label, .index = index, .first = first, .end = end, .body = body});
    }

    void flatten_stmt(ast::stmt_id id)
    {
        auto const& s = ast().at(id);
        auto const from = origin{.file = file(), .stmt = id};
        if (auto const* const let = s.node.try_as<ast::let_stmt>())
        {
            auto const* const name
                = ast::is_valid(let->pattern) ? ast().at(let->pattern).node.try_as<ast::name>() : nullptr;
            if (name == nullptr)
            {
                is_failed = true;
                return;
            }
            auto const value = flatten_expr(let->value);
            auto const local = add_local(let->is_mut ? local_kind::var : local_kind::let,
                                         c.text_of(file(), name->where), tables().type_at(let->pattern));
            current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)}, .local = local});
            if (let->is_mut)
                add_stmt(from, flat_var{.local = local, .value = value});
            else
                add_stmt(from, flat_let{.local = local, .value = value});
            return;
        }
        if (auto const* const assign = s.node.try_as<ast::assign_stmt>())
            return flatten_assign(from, *assign);
        if (auto const* const chain = s.node.try_as<ast::if_stmt>())
            return flatten_if(from, ast().at(chain->branches));
        if (auto const* const loop = s.node.try_as<ast::for_stmt>())
            return flatten_for(from, id, *loop);
        if (auto const* const loop = s.node.try_as<ast::while_stmt>())
        {
            auto const label = add_label("while");
            auto const condition = flatten_expr(loop->condition);
            current()->loops.push_back({.loop = label});
            auto const body = flatten_body(loop->body);
            current()->loops.remove_back();
            return add_stmt(from, flat_while{.label = label, .condition = condition, .body = body});
        }
        if (auto const* const print = s.node.try_as<ast::print_stmt>())
            return add_stmt(from, flat_print{.value = flatten_expr(print->message)});

        auto const* const e = s.node.try_as<ast::expr_stmt>();
        if (e == nullptr || !ast::is_valid(e->value))
        {
            is_failed = true;
            return;
        }
        flatten_expr_stmt(from, e->value);
    }

    void flatten_expr_stmt(origin from, ast::expr_id value)
    {
        auto const& x = ast().at(value);
        auto const& loops = current()->loops;
        if (auto const* const r = x.node.try_as<ast::return_expr>())
            return flatten_return(from, r->value);
        if (auto const* const b = x.node.try_as<ast::break_expr>())
        {
            if (loops.empty())
            {
                is_failed = true;
                return;
            }
            if (!ast::is_valid(b->value))
                return add_stmt(from, flat_leave{.target = loops.back().loop});
            // by value: flattening the value may open a loop of its own
            auto const target = loops.back().value_block;
            is_failed = is_failed || !is_valid(target);
            return add_stmt(from, flat_leave{.target = target, .value = flatten_expr(b->value)});
        }
        if (x.node.is<ast::continue_expr>())
        {
            if (loops.empty())
            {
                is_failed = true;
                return;
            }
            return add_stmt(from, flat_continue{.target = loops.back().loop});
        }
        if (auto const* const loop = x.node.try_as<ast::loop_expr>())
        {
            auto const label = add_label("loop");
            current()->loops.push_back({.loop = label});
            auto const body = flatten_body(loop->body);
            current()->loops.remove_back();
            return add_stmt(from, flat_loop{.label = label, .body = body});
        }
        if (auto const* const y = x.node.try_as<ast::yield_expr>())
        {
            auto const& blocks = current()->value_blocks;
            if (blocks.empty())
            {
                is_failed = true;
                return;
            }
            // by value: flattening the value may open a value block of its own
            auto const target = blocks.back();
            auto const value = flatten_expr(y->value);
            return add_stmt(from, flat_leave{.target = target, .value = value});
        }
        if (auto const* const c = x.node.try_as<ast::case_expr>())
            return add_stmt(from, flatten_case_parts(*c, label_id::none));

        // What is left is a call, and one that returns nothing is a block that is a statement.
        auto const* const call = x.node.try_as<ast::call>();
        if (call == nullptr)
        {
            is_failed = true;
            return;
        }
        auto const& where = tables().target_at(value);
        if (where.kind == target_kind::overload && !is_valid(c.out.at(where.symbol).intrinsic)
            && tables().type_at(value) == checked_module::nothing_type)
        {
            auto const arguments = flatten_arguments(call->arguments);
            auto const inlined = inline_call(value, where.symbol, arguments);
            return add_stmt(from, flat_block{.label = inlined.label, .body = inlined.body});
        }
        // Any other call has a value, which is evaluated where it stands and dropped.
        add_stmt(from, flat_eval{.value = flatten_expr(value)});
    }

    /// Far beyond any program; it bounds the work on a tree the check pass should never have let through.
    static constexpr isize k_max_inline_depth = 256;
};
} // namespace

bool checker::is_sound(type_id type) const
{
    if (!is_valid(type) || type == checked_module::error_type || type == checked_module::nothing_type)
        return false;
    for (auto const& m : out.at(out.at(type).members))
        if (!is_sound(m.type))
            return false;
    return true;
}

void checker::flatten_entry_point(symbol_id id)
{
    auto const& s = out.at(id);
    auto const& info = out.functions[s.info];
    auto const& note = notes[s.info];
    if (info.entry_stage == stage::none || !note.is_valid_entry || !inlines_whole(id))
        return;

    auto const parameter = out.at(info.parameters)[0];
    // A compute entry point hands nothing back, so `nothing` is its result and not a hole.
    auto const wants_result = info.entry_stage != stage::compute;
    if (!is_sound(parameter.type) || (wants_result && !is_sound(info.result)))
        return;

    auto f = flattener{.c = *this};
    f.entry.entry_stage = info.entry_stage;
    f.entry.name = s.name;
    f.entry.function = id;
    f.entry.input = parameter.type;
    f.entry.result = info.result;
    f.entry.workgroup[0] = info.workgroup[0];
    f.entry.workgroup[1] = info.workgroup[1];
    f.entry.workgroup[2] = info.workgroup[2];
    f.entry.takes_thread_id = parameter.is_thread_id;
    for (auto const binding : out.at(info.bindings))
        f.entry.bindings.push_back(binding);

    // Every module-level name is taken, so no local can hide a type, a binding or a builtin an emitter writes.
    f.entry.names.reserve(s.name);
    for (auto const& other : out.symbols)
        f.entry.names.reserve(other.name);

    f.frames.push_back({.function = id, .file = s.file, .result = info.result});
    auto const local = f.add_local(local_kind::parameter, parameter.name, parameter.type);
    f.current()->bound.push_back(
        {.where = {.kind = target_kind::parameter, .index = i32(parameter.field)}, .local = local});

    auto const& body = ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().body;
    if (ast::is_valid(body.value))
        f.flatten_return({.file = s.file, .expr = body.value}, body.value);
    for (auto const stmt : ast_of(s.file).at(body.statements))
        f.flatten_stmt(stmt);

    // CHK-193: known only now, since only the whole inlined body says what an entry point reaches.
    auto const stage_name = [](stage st)
    {
        return st == stage::vertex ? "vertex" : st == stage::pixel ? "pixel" : "compute";
    };
    for (auto const& v : f.stage_violations)
        report(diagnostic_kind::stage_not_allowed, v.file, span_of(v.file, v.call),
               cc::format("{} is a {} entry point, and {} is @stages without it", s.name, stage_name(info.entry_stage),
                          out.at(v.callee).name));
    if ((info.stages & stage_bit(info.entry_stage)) == 0)
        report(diagnostic_kind::stage_not_allowed, s.file, ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().name,
               cc::format("{} is a {} entry point, and its own @stages leaves that out", s.name,
                          stage_name(info.entry_stage)));
    // CHK-213: a gap of this pass is reported, so an entry point never vanishes without a word.
    if (f.is_failed && !f.meets_error)
        unsupported(s.file, ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().name,
                    cc::format("{}: its body reaches a construct the flat tree cannot hold yet", s.name));
    if (f.is_failed || !f.stage_violations.empty() || (info.stages & stage_bit(info.entry_stage)) == 0)
        return;
    f.entry.body = f.add_list(f.block);
    out.entry_points.push_back(cc::move(f.entry));
}
