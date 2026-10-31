#include <clean-core/common/utility.hh>
#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/interpret/interpret.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
bool writes_outside(flat_entry_point const& e, ast::range_of<flat_stmt_id> body, int depth);

/// True where evaluating `id` writes what outlives it: a builtin call that is not pure.
bool writes_outside(flat_entry_point const& e, flat_expr_id id, int depth)
{
    if (!is_known(e, id) || depth > k_max_depth)
        return false;
    auto const& x = e.at(id);
    if (auto const* const call = x.node.try_as<flat_call>(); call != nullptr && !call->is_pure)
        return true;
    if (auto const* const block = x.node.try_as<flat_block>())
        return writes_outside(e, block->body, depth + 1);
    auto result = false;
    for_each_operand(e, x, [&](flat_expr_id operand) { result = result || writes_outside(e, operand, depth + 1); });
    return result;
}

/// True where running `body` writes what outlives it: a print, a store to a buffer element, or an impure builtin.
/// A local is no such thing, so an inlined function that only computes a value writes nothing.
bool writes_outside(flat_entry_point const& e, ast::range_of<flat_stmt_id> body, int depth)
{
    if (!is_known(e, body) || depth > k_max_depth)
        return false;
    for (auto const id : e.at(body))
    {
        if (!is_known(e, id))
            continue;
        auto const& s = e.at(id);
        if (s.node.is<flat_print>())
            return true;
        if (auto const* const assign = s.node.try_as<flat_assign>();
            assign != nullptr && is_known(e, assign->place) && e.at(assign->place).node.is<flat_buffer_element>())
            return true;
        auto result = false;
        for_each_expr_of(s, [&](flat_expr_id x) { result = result || writes_outside(e, x, depth + 1); });
        for_each_body_of(
            e, s, [&](ast::range_of<flat_stmt_id> inner) { result = result || writes_outside(e, inner, depth + 1); });
        if (result)
            return true;
    }
    return false;
}

/// A call reached from an entry point of a stage its callee's `@stages` leaves out (CHK-193).
struct stage_violation
{
    i32 file = 0;
    ast::expr_id call = ast::expr_id::none;
    symbol_id callee = symbol_id::none;
    /// The call written in the tree's own body that reaches it, the call itself where it stands there.
    call_site written = {};
    /// `any_hit` or `intersection` for a write through a binding in a traversal's decision (CHK-404); `none` otherwise.
    stage decision = stage::none;
    /// The decision is one an inline trace calls, rather than the entry point itself.
    bool is_inlined_decision = false;
    /// The write is an assignment to a buffer's element, `call` its place; `callee` is then none.
    bool is_buffer_store = false;
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
    /// The decision an inline trace's loop is calling while its body is inlined: `any_hit` or `intersection` (CHK-404).
    stage inlined_decision = stage::none;
    /// A test's calls of a builtin only the ray-tracing stages reach, which a test cannot run yet.
    cc::vector<stage_violation> ray_stage_calls;
    /// A test's calls of a subgroup operation, which a run of one invocation has no subgroup for (CHK-379).
    cc::vector<stage_violation> subgroup_calls;
    /// A compute entry point's calls of a quad operation, whose workgroup then forms its quads along one axis (CHK-380).
    cc::vector<stage_violation> quad_calls;
    /// The condition of every `assert` whose run would write what outlives it, which its caller reports (CHK-227).
    cc::vector<origin> effectful_asserts;
    /// Every `discard` the tree reaches, which only a pixel entry point may (CHK-277).
    cc::vector<origin> discards;
    /// An unrolled `for` the tree did not unroll, which its caller reports once (CHK-389).
    /// One whose bound is no constant is an error, and one that would write too many copies is a loop instead, and a warning.
    struct unroll_report
    {
        diagnostic_kind kind = diagnostic_kind::invalid_constant_argument;
        i32 file = 0;
        source_span where;
        cc::string detail;
        /// The call the loop's function was inlined through, the innermost one; none for a loop of the tree's own body.
        call_site site = {};
    };
    cc::vector<unroll_report> unroll_reports;
    /// How many times the statement being written is written: the product of the copies of every unrolled `for` around it.
    /// It is what bounds a nest of them as a whole (CHK-389).
    i64 unrolled_copies = 1;
    /// How many copies of unrolled `for`s the statement being written stands in.
    i32 unrolled_depth = 0;
    /// The innermost unrolled `for` around the statement being written that is written as a loop, by its range.
    /// Inside it, an inner unrolled `for` whose bound reads its index has no constant bound, and is a loop too (CHK-389).
    struct looped_unroll
    {
        i32 file = 0;
        source_span where;
    };
    cc::optional<looped_unroll> looped_around;
    /// A walk of a range (CHK-415): the next `for` runs `k` over `0 ..< count`, and its variable is `start + k * step`.
    /// Taken by the `for` it was set for before anything in its body is flattened.
    struct walk_mapping
    {
        flat_expr_id start = flat_expr_id::none;
        i32 step = 1;
    };
    cc::optional<walk_mapping> pending_walk;
    /// A call of a builtin that needs a feature of the device, which the entry point then needs too (CHK-322).
    /// A value of a type that needs one, such as a `half`, counts alike (CHK-382).
    struct feature_use
    {
        i32 file = 0;
        ast::expr_id call = ast::expr_id::none;
        feature_set features;
        /// The functions whose bodies the call stands in, the tree's own first, each of which uses what it needs.
        cc::vector<symbol_id> within;
        /// A value rather than a call, which the note says.
        bool is_value = false;
    };
    cc::vector<feature_use> feature_uses;

    /// CHK-382: a value of `type` at `from` needs what the type needs, once per function it stands in.
    void note_value_use(type_id type, ast::expr_id from)
    {
        auto const needs = c.features_of_type(type);
        if (needs.is_empty() || frames.empty() || !ast::is_valid(from))
            return;
        for (auto const& u : feature_uses)
            if (u.is_value && u.features == needs && !u.within.empty() && u.within.back() == frames.back().function)
                return;
        auto within = cc::vector<symbol_id>();
        for (auto const& fr : frames)
            within.push_back(fr.function);
        feature_uses.push_back(
            {.file = file(), .call = from, .features = needs, .within = cc::move(within), .is_value = true});
    }
    /// CHK-345: an intersection entry point fused with the any hit of one record, which its own `return` runs.
    struct fusion
    {
        symbol_id any_hit = symbol_id::none;
        local_id payload = local_id::none;
        local_id box = local_id::none;
    };
    cc::optional<fusion> fused;

    /// Notes a call of `callee` whose `@stages` leaves out the stage of the entry point being flattened.
    /// A test has no stage, so it may reach what any stage may, but a builtin only the ray-tracing stages run.
    void judge_stage(ast::expr_id call, symbol_id callee)
    {
        if (auto const* const record = c.out.builtin_function(c.out.at(callee).intrinsic);
            record != nullptr && !record->features.is_empty())
        {
            auto within = cc::vector<symbol_id>();
            for (auto const& fr : frames)
                within.push_back(fr.function);
            feature_uses.push_back(
                {.file = file(), .call = call, .features = record->features, .within = cc::move(within)});
        }
        // CHK-298: a test's run is one invocation, which has no quad to take a derivative across
        if (is_test)
        {
            auto const* const record = c.out.builtin_function(c.out.at(callee).intrinsic);
            if (record != nullptr && record->uses_derivatives)
                stage_violations.push_back({.file = file(), .call = call, .callee = callee});
            auto const chain = entry.at(current()->chain);
            auto const written = chain.empty() ? call_site{.file = file(), .call = call} : chain[0];
            if (record != nullptr && record->is_subgroup_operation)
                subgroup_calls.push_back({.file = file(), .call = call, .callee = callee, .written = written});
            auto const ray_stages = stage_bit(stage::raygen) | stage_bit(stage::miss) | stage_bit(stage::closest_hit)
                                  | stage_bit(stage::any_hit) | stage_bit(stage::intersection)
                                  | stage_bit(stage::callable);
            if (auto const& s = c.out.at(callee); s.info >= 0 && (c.out.functions[s.info].stages & ~ray_stages) == 0)
                ray_stage_calls.push_back({.file = file(), .call = call, .callee = callee, .written = written});
            return;
        }
        if (entry.entry_stage == stage::none)
            return;
        auto const* const record = c.out.builtin_function(c.out.at(callee).intrinsic);
        if (record != nullptr && record->is_quad_operation && entry.entry_stage == stage::compute)
            quad_calls.push_back({.file = file(), .call = call, .callee = callee});
        // CHK-404: a decision runs as often as the traversal asks it, whichever stage's loop asks
        if (auto const decision = decision_stage();
            decision != stage::none && record != nullptr
            && (record->is_atomic || record->is_image_store || record->is_bytes_store))
        {
            stage_violations.push_back({.file = file(),
                                        .call = call,
                                        .callee = callee,
                                        .decision = decision,
                                        .is_inlined_decision = inlined_decision != stage::none});
            return;
        }
        // EMIT-163: what only a pixel stage needs of the device, which every target then judges alike
        if (record != nullptr && entry.entry_stage == stage::pixel && !record->pixel_features.is_empty())
        {
            auto within = cc::vector<symbol_id>();
            for (auto const& fr : frames)
                within.push_back(fr.function);
            feature_uses.push_back(
                {.file = file(), .call = call, .features = record->pixel_features, .within = cc::move(within)});
        }
        auto const& s = c.out.at(callee);
        if (s.info >= 0 && (c.out.functions[s.info].stages & stage_bit(entry.entry_stage)) == 0)
            stage_violations.push_back({.file = file(), .call = call, .callee = callee});
    }
    /// What a traversal decides how often to run, which then writes no memory through a binding (CHK-404): an
    /// `@any_hit` or `@intersection` entry point, or the decision an inline trace's loop is calling.
    [[nodiscard]] stage decision_stage() const
    {
        if (inlined_decision != stage::none)
            return inlined_decision;
        auto const is_decision = entry.entry_stage == stage::any_hit || entry.entry_stage == stage::intersection;
        return is_decision ? entry.entry_stage : stage::none;
    }

    /// CHK-404: an assignment to a buffer's element, or to a part of one, from a decision.
    void judge_decision_store(flat_expr_id place, ast::expr_id target)
    {
        auto const decision = decision_stage();
        for (auto depth = 0; decision != stage::none && is_valid(place) && depth < k_max_inline_depth; ++depth)
        {
            auto const& node = entry.at(place).node;
            if (node.is<flat_buffer_element>())
            {
                stage_violations.push_back({.file = file(),
                                            .call = target,
                                            .decision = decision,
                                            .is_inlined_decision = inlined_decision != stage::none,
                                            .is_buffer_store = true});
                return;
            }
            if (auto const* const member = node.try_as<flat_member>())
                place = member->object;
            else if (auto const* const element = node.try_as<flat_element>())
                place = element->object;
            else
                return;
        }
    }

    flat_entry_point entry;
    /// The tree is a test, whose own lines of type bool are checks (CHK-225); a function it calls has none.
    bool is_test = false;
    /// The index of every `for` of the test's own body around the statement being written, outermost first.
    cc::vector<local_id> test_loops;
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
        /// A parameter of function type: a position in `closures`, the function a call of the parameter inlines.
        i32 closure = -1;
    };

    /// A function handed to a parameter of function type (CHK-318): a function of the program, or a lambda with the
    /// names it sees where it was written.
    struct closure
    {
        symbol_id function = symbol_id::none;
        i32 file = 0;
        ast::expr_id lambda = ast::expr_id::none;
        cc::vector<bound_name> bound;
        ast::range_of<call_site> chain;
        /// The function the lambda was written in.
        symbol_id owner = symbol_id::none;
        /// What the type parameters it sees stood for where it was written.
        cc::vector<type_id> bindings;
    };
    cc::vector<closure> closures;
    /// Parallel to what `flatten_written` returned last: the closure a function argument hands over, -1 elsewhere.
    cc::vector<i32> written_closures;

    /// A loop of the source around the statement being written.
    struct loop_target
    {
        /// What a `continue` names, and a `break` without a value.
        label_id loop = label_id::none;
        /// The block around a `loop:` that is a value, which a `break value` leaves.
        label_id value_block = label_id::none;
        /// The copy of an unrolled `for`'s body being written, which a `continue` leaves rather than naming `loop`.
        label_id continue_block = label_id::none;
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
        /// What the type parameters of a generic function stand for in this inlining, two by two (CHK-340).
        cc::vector<type_id> bindings;
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
        mark_failed();
        return flat_expr_id::none;
    }

    /// The expression or statement being flattened, innermost, which a failure is located at (CHK-213).
    origin cursor;
    /// Where the first failure stood: what a report names, however deep the construct is.
    cc::optional<origin> first_failure;

    void mark_failed()
    {
        is_failed = true;
        if (!first_failure.has_value() && (ast::is_valid(cursor.expr) || ast::is_valid(cursor.stmt)))
            first_failure = cursor;
    }

    /// Moves `cursor` to `at` until it goes out of scope.
    struct cursor_scope
    {
        flattener& f;
        origin outer;
        cursor_scope(flattener& f, origin at) : f(f), outer(f.cursor) { f.cursor = at; }
        ~cursor_scope() { f.cursor = outer; }
        cursor_scope(cursor_scope const&) = delete;
        cursor_scope& operator=(cursor_scope const&) = delete;
    };

    /// `type` as the current frame's bindings make it: a generic body names its type parameters, and the tree never does.
    [[nodiscard]] type_id concrete(type_id type)
    {
        if (frames.empty() || frames.back().bindings.empty())
            return type;
        auto const result = c.substitute_existing(type, frames.back().bindings);
        if (!is_valid(result) || c.is_open(result))
        {
            mark_failed();
            return type;
        }
        return result;
    }

    local_id add_local(local_kind kind, cc::string_view desired, type_id type)
    {
        type = concrete(type);
        is_failed = is_failed || !c.is_sound(type);
        meets_error = meets_error || holds_error(type);
        // `_` is a name nobody reads, and no name at all in WGSL
        entry.locals.push_back({.kind = kind,
                                .name = entry.names.mint(desired == "_" ? cc::string_view("unused") : desired),
                                .type = type,
                                .is_mut = kind == local_kind::var});
        return local_id(entry.locals.size() - 1);
    }

    /// The names of `entry.labels`, which are unique among the labels alone.
    name_mint label_names;

    label_id add_label(cc::string_view desired)
    {
        entry.labels.push_back({.name = label_names.mint(desired)});
        return label_id(entry.labels.size() - 1);
    }

    template <class Node>
    flat_expr_id add_expr(type_id type, ast::expr_id from, Node node)
    {
        type = concrete(type);
        note_value_use(type, from);
        // A builtin with an effect may give nothing, `store`, and its call is only ever an `eval`'s value.
        auto const is_effect_call = std::is_same_v<Node, flat_call> && type == checked_module::void_type;
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

    /// The constant arrays this tree reads, each bound once to a local of its own (EMIT-165).
    struct hoisted_constant
    {
        symbol_id symbol = symbol_id::none;
        local_id local = local_id::none;
    };
    cc::vector<hoisted_constant> hoisted_constants;
    /// Their `let`s, which stand in front of everything else of the tree's body.
    cc::vector<flat_stmt_id> hoisted_lets;

    /// The tree's own body: the hoisted constants' `let`s, then what was written.
    ast::range_of<flat_stmt_id> root_body()
    {
        auto all = hoisted_lets;
        for (auto const stmt : block)
            all.push_back(stmt);
        return add_list(all);
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

    /// What `nonuniform i` or `uniform i` marks, `i`; none for anything else.
    [[nodiscard]] flat_expr_id marked_index(flat_expr_id id) const
    {
        auto const* const call = is_valid(id) ? entry.at(id).node.try_as<flat_call>() : nullptr;
        auto const* const record = call != nullptr ? c.out.builtin_function(call->intrinsic) : nullptr;
        return record != nullptr && record->mark != builtins::index_mark::none ? entry.at(call->arguments)[0]
                                                                               : flat_expr_id::none;
    }

    /// An index that may stand in several places: a substitutable one, or a mark of one, the mark kept on it.
    [[nodiscard]] bool is_substitutable_index(flat_expr_id id) const
    {
        return is_substitutable(id) || is_substitutable(marked_index(id));
    }

    /// A construction of literals alone, such as a texel offset, which a target may take only as it is written.
    [[nodiscard]] bool is_literal_construction(flat_expr_id id) const
    {
        auto const* const made = is_valid(id) ? entry.at(id).node.try_as<flat_construct>() : nullptr;
        if (made == nullptr)
            return false;
        for (auto const a : entry.at(made->arguments))
        {
            auto const& x = entry.at(a).node;
            auto const is_literal = x.is<flat_literal>() || x.is<flat_int_literal>() || x.is<flat_bool_literal>()
                                 || x.is<flat_enum_value>();
            if (!is_literal && !is_literal_construction(a))
                return false;
        }
        return true;
    }

    /// One more node that means what the substitutable `id`, or a construction of literals, means, attributed to `from`.
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
        if (auto const* const m = x.node.try_as<flat_binding_member>())
            return add_expr(x.type, from, *m);
        if (auto const* const smp = x.node.try_as<flat_file_sampler>())
            return add_expr(x.type, from, *smp);
        if (auto const* const element = x.node.try_as<flat_element>())
        {
            auto const object = again(element->object, from);
            auto const index = again_as_written(element->index, from);
            return add_expr(x.type, from, flat_element{.object = object, .index = index});
        }
        // a place a `mut` parameter stands for, whose indices were pinned when it was bound (`pin_place`)
        if (auto const* const member = x.node.try_as<flat_member>())
        {
            auto const index = member->member;
            auto const letters = member->letters;
            auto const object = again(member->object, from);
            return add_expr(x.type, from, flat_member{.object = object, .member = index, .letters = letters});
        }
        if (auto const* const element = x.node.try_as<flat_buffer_element>())
        {
            auto const buffer = again(element->buffer, from);
            auto const index = again_as_written(element->index, from);
            return add_expr(x.type, from, flat_buffer_element{.buffer = buffer, .index = index});
        }
        // the mark stays where it was written, which is what a diagnostic about it points at
        if (auto const marked = marked_index(id); is_valid(marked))
        {
            flat_expr_id const arguments[] = {again(marked, from)};
            auto copy = x;
            copy.node.as<flat_call>().arguments = add_list(arguments);
            entry.exprs.push_back(cc::move(copy));
            return flat_expr_id(entry.exprs.size() - 1);
        }
        if (auto const* const made = x.node.try_as<flat_construct>())
        {
            // copied first: `again` appends to the lists the view reads
            auto arguments = cc::vector<flat_expr_id>();
            for (auto const a : entry.at(made->arguments))
                arguments.push_back(a);
            for (auto& a : arguments)
                a = again(a, from);
            return add_expr(x.type, from, flat_construct{.arguments = add_list(arguments)});
        }
        // the int arithmetic a walk's variable is (CHK-415), whose operands are substitutable in turn
        if (auto const* const call = x.node.try_as<flat_call>())
        {
            auto arguments = cc::vector<flat_expr_id>();
            for (auto const a : entry.at(call->arguments))
                arguments.push_back(a);
            for (auto& a : arguments)
                a = again(a, from);
            auto copy = x;
            copy.node.as<flat_call>().arguments = add_list(arguments);
            entry.exprs.push_back(cc::move(copy));
            return flat_expr_id(entry.exprs.size() - 1);
        }
        return fail();
    }

    /// `index` as it may stand in several places: itself where it is substitutable, and otherwise a local of `kind`
    /// bound at `from`.
    /// A marked `i` binds `i` and marks the bound local, since a mark is read only where it indexes (CHK-300).
    flat_expr_id bound_index(flat_expr_id index, local_kind kind, cc::string_view name, origin from)
    {
        if (is_substitutable_index(index))
            return index;
        auto const marked = marked_index(index);
        auto const value = is_valid(marked) ? marked : index;
        auto const local = add_local(kind, name, entry.at(value).type);
        add_stmt(from, flat_let{.local = local, .value = value});
        if (!is_valid(marked))
            return local_ref(local, from.expr);
        flat_expr_id const arguments[] = {local_ref(local, from.expr)};
        auto mark = entry.at(index);
        mark.node.as<flat_call>().arguments = add_list(arguments);
        entry.exprs.push_back(cc::move(mark));
        return flat_expr_id(entry.exprs.size() - 1);
    }

    /// `again` for an index, which keeps the place it was written: a diagnostic about the index of an element a
    /// parameter stands for belongs to the caller that wrote it, not to wherever the callee names the parameter.
    flat_expr_id again_as_written(flat_expr_id index, ast::expr_id from)
    {
        if (!is_substitutable_index(index))
            return again(index, from);
        // by value: the copy of a mark's argument appends to the arrays
        auto copy = entry.at(index);
        if (auto const marked = marked_index(index); is_valid(marked))
        {
            flat_expr_id const arguments[] = {again_as_written(marked, from)};
            copy.node.as<flat_call>().arguments = add_list(arguments);
        }
        entry.exprs.push_back(cc::move(copy));
        return flat_expr_id(entry.exprs.size() - 1);
    }

    /// `place` with every index it holds evaluated now, into a local where it is no substitutable value, so the place
    /// can stand wherever a `mut` parameter is named and mean the same element each time (CHK-316).
    flat_expr_id pin_place(flat_expr_id place)
    {
        auto const x = entry.at(place);
        auto const pin_index
            = [&](flat_expr_id index) { return bound_index(index, local_kind::let, "at", entry.at(index).from); };
        if (auto const* const member = x.node.try_as<flat_member>())
        {
            auto const index = member->member;
            auto const letters = member->letters;
            auto const object = pin_place(member->object);
            return add_expr(x.type, x.from.expr, flat_member{.object = object, .member = index, .letters = letters});
        }
        if (auto const* const element = x.node.try_as<flat_element>())
        {
            auto const index = element->index;
            auto const object = pin_place(element->object);
            return add_expr(x.type, x.from.expr, flat_element{.object = object, .index = pin_index(index)});
        }
        if (auto const* const element = x.node.try_as<flat_buffer_element>())
        {
            auto const index = element->index;
            auto const buffer = element->buffer;
            return add_expr(x.type, x.from.expr, flat_buffer_element{.buffer = buffer, .index = pin_index(index)});
        }
        return place;
    }

    /// True for a texture, an image, a sampler or a buffer read from its binding, and a file-scope sampler: it is no
    /// value a local could hold, and it stands wherever it is named, since naming one has no effect.
    [[nodiscard]] bool is_resource_member(flat_expr_id id) const
    {
        if (!is_valid(id) || !is_resource(c.out.at(entry.at(id).type).kind))
            return false;
        // an element of a binding array, at an index that reads the same wherever it stands
        if (auto const* const element = entry.at(id).node.try_as<flat_element>())
            return entry.at(element->object).node.is<flat_binding_member>() && is_substitutable_index(element->index);
        return entry.at(id).node.is<flat_binding_member>() || entry.at(id).node.is<flat_file_sampler>();
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

    // ---- swizzles ---------------------------------------------------------------------------------------------------

    /// The fields of `operand` a swizzle reads.
    struct swizzled
    {
        ast::expr_id operand = ast::expr_id::none;
        swizzle letters;
    };

    /// True for the member `id` that is a swizzle, or a field of one.
    [[nodiscard]] bool is_swizzled(ast::expr_id id) const
    {
        auto const* const m = ast::is_valid(id) ? ast().at(id).node.try_as<ast::member>() : nullptr;
        if (m == nullptr)
            return false;
        auto const kind = tables().target_at(id).kind;
        return kind == target_kind::swizzle
            || (kind == target_kind::field && ast::is_valid(m->object)
                && tables().target_at(m->object).kind == target_kind::swizzle);
    }

    /// What the `is_swizzled` member `id` reads, through every swizzle below it: `v.zyx.yx` reads `v.yz`, and `v.zy.x`
    /// reads `v.z`.
    [[nodiscard]] swizzled swizzled_at(ast::expr_id id) const
    {
        auto const& where = tables().target_at(id);
        auto result = swizzled{
            .operand = ast().at(id).node.as<ast::member>().object,
            .letters = where.kind == target_kind::swizzle ? swizzle::unpacked(where.index)
                                                          : swizzle{.fields = {i8(where.index)}, .count = 1},
        };
        while (tables().target_at(result.operand).kind == target_kind::swizzle)
        {
            result.letters = result.letters.over(swizzle::unpacked(tables().target_at(result.operand).index));
            result.operand = ast().at(result.operand).node.as<ast::member>().object;
        }
        return result;
    }

    /// The type of field `field` of a value of `type`.
    [[nodiscard]] type_id field_type(type_id type, i32 field) const
    {
        return c.out.at(c.out.at(type).members)[field].type;
    }

    /// One field of `object`, or the swizzle `letters` of it, which is `type`.
    /// A prelude vector's swizzle is a member of its own; a struct of the program's is the construction of the vector it
    /// means, from an operand that is no local evaluated once (EMIT-142).
    flat_expr_id swizzle_of(flat_expr_id object, swizzle letters, type_id type, ast::expr_id id)
    {
        if (!is_valid(object))
            return fail();
        auto const object_type = entry.at(object).type;
        if (letters.count == 1)
            return add_expr(type, id, flat_member{.object = object, .member = letters.fields[0]});
        if (c.out.builtin_type_of(object_type) != nullptr)
            return add_expr(type, id, flat_member{.object = object, .letters = letters});
        auto const once = entry.at(object).node.is<flat_local_ref>() ? evaluated_once{.first = object, .later = object}
                                                                     : evaluate_once(object, "swizzled", id);
        auto arguments = cc::vector<flat_expr_id>();
        for (auto i = 0; i < letters.count; ++i)
        {
            auto const operand = i == 0 ? once.first : again(once.later, id);
            arguments.push_back(add_expr(field_type(object_type, letters.fields[i]), id,
                                         flat_member{.object = operand, .member = letters.fields[i]}));
        }
        return add_expr(type, id, flat_construct{.arguments = add_list(arguments)});
    }

    /// CHK-391: `value as texel[F]` of the texel type `type` and of another width: its leading components, or
    /// all of them and the rest of (0, 0, 0, 1), from an operand that is evaluated once.
    flat_expr_id texel_conversion(flat_expr_id value, type_id type, ast::expr_id id)
    {
        if (!is_valid(value))
            return fail();
        auto const value_type = entry.at(value).type;
        auto const* const source = c.out.builtin_type_of(value_type);
        auto const* const texel = c.out.builtin_type_of(type);
        if (source == nullptr || texel == nullptr)
            return fail();
        auto const from_width = source->leaf_count;
        auto const to_width = texel->leaf_count;
        if (to_width < from_width)
        {
            auto letters = swizzle{.count = i8(to_width)};
            for (auto i = 0; i < to_width; ++i)
                letters.fields[i] = i8(i);
            return swizzle_of(value, letters, type, id);
        }

        // a widened texel is a vector, whose fields are its component
        auto const component = field_type(type, 0);
        auto arguments = cc::vector<flat_expr_id>();
        if (from_width == 1)
            arguments.push_back(value);
        else
        {
            auto const once = evaluate_once(value, "texel", id);
            for (auto i = 0; i < from_width; ++i)
            {
                auto const operand = i == 0 ? once.first : again(once.later, id);
                arguments.push_back(add_expr(component, id, flat_member{.object = operand, .member = i}));
            }
        }
        for (auto i = from_width; i < to_width; ++i)
        {
            auto const fill = i == 3 ? 1 : 0;
            if (texel->leaf_kind == value_kind::scalar_float)
                arguments.push_back(add_expr(component, id, flat_literal{.value = f64(fill)}));
            else
                arguments.push_back(add_expr(
                    component, id,
                    flat_int_literal{.value = fill, .is_unsigned = texel->leaf_kind == value_kind::scalar_uint}));
        }
        return add_expr(type, id, flat_construct{.arguments = add_list(arguments)});
    }

    flat_expr_id read_swizzle(swizzled const& s, type_id type, ast::expr_id id)
    {
        return swizzle_of(flatten_expr(s.operand), s.letters, type, id);
    }

    /// `v.zy = value`, and `v.zy op= value`, whose operand's indices are evaluated once, before the value (CHK-352).
    void flatten_swizzle_assign(origin from, ast::assign_stmt const& assign, cc::string_view op)
    {
        auto const s = swizzled_at(assign.target);
        auto const type = tables().type_at(assign.target);
        auto const operand = pin_place(flatten_expr(s.operand));
        auto value = flatten_expr(assign.value);
        if (!is_valid(operand) || !is_valid(value))
        {
            mark_failed();
            return;
        }
        judge_decision_store(operand, assign.target);
        auto const operand_type = entry.at(operand).type;
        if (op != "=")
        {
            type_id const types[] = {type, entry.at(value).type};
            auto const callee = c.find_operator(file(), op.subview({.offset = 0, .size = op.size() - 1}), types);
            if (!is_valid(callee))
            {
                mark_failed();
                return;
            }
            auto const read = swizzle_of(again(operand, assign.target), s.letters, type, assign.target);
            flat_expr_id const arguments[] = {read, value};
            value = builtin_call(assign.value, callee, arguments);
        }
        if (s.letters.count == 1 || c.out.builtin_type_of(operand_type) != nullptr)
        {
            add_stmt(from, flat_assign{.place = swizzle_of(operand, s.letters, type, assign.target), .value = value});
            return;
        }
        // EMIT-143: a struct of the program takes one component at a time, from the value held once
        auto const held = add_local(local_kind::temporary, "swizzled", type);
        add_stmt(from, flat_let{.local = held, .value = value});
        for (auto i = 0; i < s.letters.count; ++i)
        {
            auto const element = field_type(operand_type, s.letters.fields[i]);
            auto const object = i == 0 ? operand : again(operand, assign.target);
            auto const place
                = add_expr(element, assign.target, flat_member{.object = object, .member = s.letters.fields[i]});
            auto const component
                = add_expr(element, assign.target, flat_member{.object = local_ref(held, assign.target), .member = i});
            add_stmt(from, flat_assign{.place = place, .value = component});
        }
    }

    // ---- expressions ------------------------------------------------------------------------------------------------

    flat_expr_id flatten_expr(ast::expr_id id)
    {
        if (!ast::is_valid(id))
            return fail();
        auto const at = cursor_scope(*this, origin{.file = file(), .expr = id});
        auto const& e = ast().at(id);
        auto const type = tables().type_at(id);
        auto const& where = tables().target_at(id);
        meets_error = meets_error || holds_error(type);
        if (!is_valid(type))
            return fail();

        if (e.node.is<ast::literal>())
            return flatten_number(id, type);
        // CHK-394: an operator over number literals alone is the literal it folds to
        if (e.node.is<ast::call>())
        {
            if (auto const n = c.number_of(file(), id); n.is_number)
                return flatten_folded(id, type, n);
            if (auto const b = c.comparison_of(file(), id); b.has_value())
                return add_expr(type, id, flat_bool_literal{.value = b.value()});
        }
        // a tuple or an object literal is the call it converts by (CHK-81)
        if ((e.node.is<ast::tuple>() || e.node.is<ast::object>()) && tables().call_at(id) >= 0)
            return flatten_bound_call(id, type, c.out.call_records[tables().call_at(id)]);
        // void's one value is the construction of no fields.
        if (e.node.is<ast::void_ref>())
            return add_expr(type, id, flat_construct{});
        if (e.node.is<ast::leading_dot>())
        {
            return where.kind == target_kind::enum_case ? enum_value(type, id, where.index) : fail();
        }
        // A module-level name: a const is its value, and a file-scope sampler is handed to a builtin.
        // A const that did not check has no value, and its name already has the error type (CHK-19).
        auto const module_level = [&]() -> flat_expr_id
        {
            if (where.kind == target_kind::symbol && c.out.at(where.symbol).kind == symbol_kind::constant
                && c.out.at(where.symbol).state == symbol_state::checked)
                return constant_value(type, id, c.out.constants[c.out.at(where.symbol).info]);
            if (where.kind == target_kind::symbol && c.out.at(where.symbol).kind == symbol_kind::sampler
                && c.out.at(where.symbol).state == symbol_state::checked)
                return add_expr(type, id, flat_file_sampler{.sampler = where.symbol});
            return fail();
        };
        if (e.node.is<ast::name>() || e.node.is<ast::self_ref>())
        {
            for (auto const& b : current()->bound)
                if (b.where == where)
                    return is_valid(b.literal) ? again(b.literal, id) : local_ref(b.local, id);
            return module_level();
        }
        if (auto const* const m = e.node.try_as<ast::member>())
        {
            // CHK-348: `m.name`, a module-level name of module `m`
            if (where.kind == target_kind::symbol)
                return module_level();
            // `a.foo` that is no field is a call of `foo` with `a` (CHK-249)
            if (tables().call_at(id) >= 0)
                return flatten_bound_call(id, type, c.out.call_records[tables().call_at(id)]);
            if (where.kind == target_kind::enum_case)
                return enum_value(type, id, where.index);
            if (where.kind == target_kind::binding_member)
                return add_expr(type, id,
                                flat_binding_member{.binding = where.symbol,
                                                    .member = where.index,
                                                    .is_workgroup = c.is_workgroup_binding(where.symbol)});
            // CHK-288: a constant, and an object with an effect still runs for it
            if (where.kind == target_kind::array_length)
            {
                auto const object = flatten_expr(m->object);
                if (calls_impure(object, 0))
                    add_stmt({.file = file(), .expr = id}, flat_eval{.value = object});
                return add_expr(type, id, flat_int_literal{.value = where.index});
            }
            if (is_swizzled(id))
                return read_swizzle(swizzled_at(id), type, id);
            if (where.kind != target_kind::field)
                return fail();
            auto const object = flatten_expr(m->object);
            return add_expr(type, id, flat_member{.object = object, .member = where.index});
        }
        if (auto const* const indexed = e.node.try_as<ast::index>())
        {
            auto const arguments = ast().at(indexed->arguments);
            auto const object_type = tables().type_at(indexed->object);
            // CHK-367: a texel read is the `load` the check pass resolved it as
            if (is_valid(object_type) && c.out.at(object_type).kind == type_kind::image)
                return tables().call_at(id) >= 0 && !c.out.at(object_type).is_atomic
                         ? flatten_bound_call(id, type, c.out.call_records[tables().call_at(id)])
                         : fail();
            // CHK-395: a lane of a vector is an element of it, which every target subscripts as it does an array
            if (auto const lane = is_valid(object_type) ? c.lane_type_of(object_type) : type_id::none;
                is_valid(lane) && arguments.size() == 1)
                return add_expr(
                    lane, id,
                    flat_element{.object = flatten_expr(indexed->object), .index = flatten_expr(arguments[0].value)});
            // CHK-287: one element per index, `grid[i, j]` being `grid[i][j]`
            if (is_valid(object_type) && c.out.at(object_type).kind == type_kind::array)
            {
                auto object = flatten_expr(indexed->object);
                auto element_type = object_type;
                for (auto const& a : arguments)
                {
                    element_type = c.out.at(element_type).element;
                    auto const index = flatten_expr(a.value);
                    object = add_expr(element_type, id, flat_element{.object = object, .index = index});
                }
                return object;
            }
            // The check pass let only a buffer element through otherwise, so the object is a resource.
            if (arguments.size() != 1)
                return fail();
            auto const buffer = flatten_expr(indexed->object);
            auto const index = flatten_expr(arguments[0].value);
            return add_expr(type, id, flat_buffer_element{.buffer = buffer, .index = index});
        }
        if (auto const* const literal = e.node.try_as<ast::array>())
            return flatten_array_literal(id, type, *literal);
        if (auto const* const call = e.node.try_as<ast::call>())
            return flatten_call(id, type, where, *call);
        if (auto const* const cast = e.node.try_as<ast::cast>())
        {
            auto const value = flatten_expr(cast->value);
            if (where.kind == target_kind::texel_conversion)
                return texel_conversion(value, type, id);
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
        if (auto const* const i = e.node.try_as<ast::if_expr>())
            return flatten_value_if(id, type, *i);
        return fail();
    }

    /// A case of `type`; a case of a builtin enum is a literal of what the targets write it as: a bool, or an int.
    flat_expr_id enum_value(type_id type, ast::expr_id from, i32 case_index)
    {
        if (c.out.is_plain_enum(type))
            return add_expr(type, from, flat_enum_value{.case_index = case_index});
        auto const cases = c.out.at(c.out.at(type).cases);
        if (case_index < 0 || case_index >= cases.size())
            return fail();
        auto const value = cases[case_index].value;
        if (c.out.builtin_type_of(type)->leaf_kind == value_kind::boolean)
            return add_expr(type, from, flat_bool_literal{.value = value != 0});
        return add_expr(type, from, flat_int_literal{.value = i32(value)});
    }

    /// A const stands for its value, written where the name stood.
    flat_expr_id constant_value(type_id type, ast::expr_id from, constant_info const& info)
    {
        switch (info.kind)
        {
        case constant_kind::integer:
            return add_expr(
                type, from,
                flat_int_literal{.value = info.integer, .is_unsigned = info.is_unsigned, .is_hex = info.is_hex});
        case constant_kind::real:
            return add_expr(type, from, flat_literal{.value = info.real});
        case constant_kind::enum_case:
            return enum_value(type, from, info.case_index);
        case constant_kind::composite:
        {
            // EMIT-165: an array is bound once per tree and read from there, since a target would write it whole at
            // every index otherwise
            auto const is_array
                = c.out.builtin_type_of(info.type) == nullptr && c.out.at(info.type).kind == type_kind::array;
            if (is_array)
                for (auto const& h : hoisted_constants)
                    if (h.symbol == info.symbol)
                        return local_ref(h.local, from);
            auto at = isize(0);
            auto const result = value_of(info.type, from, info.leaves, at);
            if (at != info.leaves.size())
                return fail();
            if (!is_array)
                return result;
            auto const local = add_local(local_kind::let, c.out.at(info.symbol).name, info.type);
            hoisted_lets.push_back(make_stmt({.file = file(), .expr = from}, flat_let{.local = local, .value = result}));
            hoisted_constants.push_back({.symbol = info.symbol, .local = local});
            return local_ref(local, from);
        }
        }
        return fail();
    }

    /// The scalars of a computed const from `at` on, built back into a value of `type` in the shape every target
    /// constructs one: a vector of its scalars, a matrix of its columns, a struct of its members and an array of its
    /// elements.
    flat_expr_id value_of(type_id type, ast::expr_id from, cc::span<scalar const> leaves, isize& at)
    {
        if (auto const* const record = c.out.builtin_type_of(type))
        {
            if (record->leaf_count == 1)
            {
                if (at >= leaves.size())
                    return fail();
                auto const leaf = leaves[at++];
                switch (leaf.kind)
                {
                case value_kind::scalar_float:
                case value_kind::scalar_half:
                    return add_expr(type, from, flat_literal{.value = f64(leaf.widened().as_float())});
                case value_kind::scalar_int:
                case value_kind::scalar_short:
                    return add_expr(type, from, flat_int_literal{.value = leaf.widened().as_int()});
                case value_kind::scalar_uint:
                case value_kind::scalar_ushort:
                    return add_expr(type, from, flat_int_literal{.value = leaf.widened().as_int(), .is_unsigned = true});
                case value_kind::boolean:
                    return add_expr(type, from, flat_bool_literal{.value = leaf.as_bool()});
                default:
                    return fail();
                }
            }
            // a vector has its components as fields, and a matrix, a `mat2` among them, has none
            if (c.out.at(type).members.count > 0)
            {
                auto arguments = cc::vector<flat_expr_id>();
                for (auto i = 0; i < record->leaf_count; ++i)
                    arguments.push_back(value_of(field_type(type, i), from, leaves, at));
                return add_expr(type, from, flat_construct{.arguments = add_list(arguments)});
            }
            // a matrix is its constructor of columns, `mat4(c0, c1, c2, c3)`, which every target writes as it must
            auto const* const overloads = c.prelude_names.get_ptr(c.out.name_of(type));
            if (overloads == nullptr)
                return fail();
            for (auto const callee : *overloads)
            {
                auto const& s = c.out.at(callee);
                if (s.kind != symbol_kind::function || s.info < 0)
                    continue;
                auto const& f = c.out.functions[s.info];
                auto const parameters = c.out.at(f.parameters);
                // the constructor of columns has as many as each column has rows
                if (f.result != type || parameters.size() * parameters.size() != record->leaf_count)
                    continue;
                auto columns = cc::vector<flat_expr_id>();
                for (auto const& p : parameters)
                    columns.push_back(value_of(p.type, from, leaves, at));
                return builtin_call(from, callee, columns);
            }
            return fail();
        }
        auto const& t = c.out.at(type);
        if (t.kind == type_kind::enumeration)
        {
            if (at >= leaves.size())
                return fail();
            auto const leaf = leaves[at++];
            auto const cases = c.out.at(t.cases);
            for (auto i = isize(0); i < cases.size(); ++i)
                if (cases[i].value == leaf.as_int())
                    return enum_value(type, from, i32(i));
            return fail();
        }
        auto arguments = cc::vector<flat_expr_id>();
        if (t.kind == type_kind::array)
            for (auto i = 0; i < t.count; ++i)
                arguments.push_back(value_of(t.element, from, leaves, at));
        else if (t.kind == type_kind::structure)
            for (auto const& member : c.out.at(t.members))
                arguments.push_back(value_of(member.type, from, leaves, at));
        else
            return fail();
        return add_expr(type, from, flat_construct{.arguments = add_list(arguments)});
    }

    /// A number literal as the type it was checked as, which a conversion may have made other than its default.
    flat_expr_id flatten_number(ast::expr_id id, type_id type)
    {
        auto text = c.text_of(file(), c.span_of(file(), id));
        // CHK-357: a suffix named the type already, and the value is the number before it
        if (auto const suffixed = split_suffix(text); suffixed.has_value())
            text = suffixed.value().body;
        auto const is_half = type == c.prelude_type(builtins::k_half);
        auto const is_float = is_half || type == c.prelude_type(builtins::k_float);
        // CHK-253: a half literal is the half nearest its value, which every target's text then spells exactly
        auto const literal = [&](f64 v)
        { return add_expr(type, id, flat_literal{.value = is_half ? f64(scalar::of_half(v).widened().as_float()) : v}); };
        if (classify_number(text) == number_class::plain_integer)
        {
            auto const value = parse_literal_integer(text);
            if (!value.has_value())
                return fail();
            if (is_float)
                return literal(f64(value.value()));
            // an unsigned literal keeps its bits in `value`
            auto const is_unsigned
                = type == c.prelude_type(builtins::k_uint) || type == c.prelude_type(builtins::k_ushort);
            auto const v = value.value();
            if (is_unsigned ? v < 0 || v > 4294967295ll : v < -2147483647 - 1 || v > 2147483647)
                return fail();
            return add_expr(
                type, id,
                flat_int_literal{.value = i32(u32(v)), .is_unsigned = is_unsigned, .is_hex = is_hex_literal(text)});
        }
        auto const value = parse_plain_float(text);
        return value.has_value() ? literal(value.value()) : fail();
    }

    /// What an operator over number literals folded to, as the type it was checked as.
    flat_expr_id flatten_folded(ast::expr_id id, type_id type, number_literal const& n)
    {
        auto const is_half = type == c.prelude_type(builtins::k_half);
        auto const is_float = is_half || type == c.prelude_type(builtins::k_float);
        if (n.is_integer && !is_float)
        {
            auto const is_unsigned
                = type == c.prelude_type(builtins::k_uint) || type == c.prelude_type(builtins::k_ushort);
            auto const v = n.integer;
            if (is_unsigned ? v < 0 || v > 4294967295ll : v < -2147483647 - 1 || v > 2147483647)
                return fail();
            return add_expr(type, id, flat_int_literal{.value = i32(u32(v)), .is_unsigned = is_unsigned});
        }
        if (!is_float)
            return fail();
        auto const v = n.is_integer ? f64(n.integer) : n.real;
        return add_expr(type, id, flat_literal{.value = is_half ? f64(scalar::of_half(v).widened().as_float()) : v});
    }

    /// `[a, b, c]` of an array type, its elements in the order written (EVAL-91).
    flat_expr_id flatten_array_literal(ast::expr_id id, type_id type, ast::array const& literal)
    {
        auto values = cc::vector<flat_expr_id>();
        for (auto const& element : ast().at(literal.elements))
            values.push_back(flatten_expr(element.value));
        // a target may build one in any order, so two with an effect run first, in order
        auto with_effect = 0;
        for (auto const v : values)
            with_effect += calls_impure(v, 0) ? 1 : 0;
        if (with_effect >= 2)
            for (auto& v : values)
                if (calls_impure(v, 0))
                {
                    auto const local = add_local(local_kind::temporary, "element", entry.at(v).type);
                    add_stmt({.file = file(), .expr = id}, flat_let{.local = local, .value = v});
                    v = local_ref(local, id);
                }
        return add_expr(type, id, flat_construct{.arguments = add_list(values)});
    }

    /// CHK-289: `T[N].filled(v)` is a local every element of which is assigned `v`, evaluated once.
    /// A nested array filled with its innermost element assigns `v` to every scalar, one loop per level.
    flat_expr_id flatten_filled(ast::expr_id id, type_id type, ast::call const& call)
    {
        auto const arguments = ast().at(call.arguments);
        if (arguments.size() != 1 || !is_valid(type))
            return fail();
        auto const where = origin{.file = file(), .expr = id};
        auto const value = flatten_expr(arguments[0].value);
        if (!is_valid(value))
            return fail();
        auto const value_type = entry.at(value).type;

        auto const label = add_label("filled");
        auto body = cc::vector<flat_stmt_id>();
        auto filler = value;
        if (!is_substitutable(value))
        {
            auto const held = add_local(local_kind::let, "fill", value_type);
            body.push_back(make_stmt(where, flat_let{.local = held, .value = value}));
            filler = local_ref(held, id);
        }
        auto const result = add_local(local_kind::var, "filled", type);
        body.push_back(make_stmt(where, flat_var{.local = result}));
        // the statements that assign `filler` to every element of `place`, of type `t`, down to the filler's own type
        auto const fill = [&](auto const& self, flat_expr_id place, type_id t) -> flat_stmt_id
        {
            if (t == value_type)
                return make_stmt(where, flat_assign{.place = place, .value = filler});
            auto const& info = c.out.at(t);
            auto const index = add_local(local_kind::index, "i", int_type());
            auto const element = add_expr(info.element, id, flat_element{.object = place, .index = local_ref(index, id)});
            flat_stmt_id const store[] = {self(self, element, info.element)};
            return make_stmt(where, flat_for{.label = add_label("fill"),
                                             .index = index,
                                             .first = add_expr(int_type(), id, flat_int_literal{.value = 0}),
                                             .end = add_expr(int_type(), id, flat_int_literal{.value = info.count}),
                                             .body = add_list(store)});
        };
        body.push_back(fill(fill, local_ref(result, id), type));
        body.push_back(make_stmt(where, flat_leave{.target = label, .value = local_ref(result, id)}));
        return add_expr(type, id, flat_block{.label = label, .body = add_list(body)});
    }

    flat_expr_id flatten_call(ast::expr_id id, type_id type, target const& where, ast::call const& call)
    {
        if (where.kind == target_kind::array_filled)
            return flatten_filled(id, type, call);
        // CHK-341: `undefined()` is a local declared and never assigned, whose value nobody reads
        if (where.kind == target_kind::undefined_value)
        {
            auto const local = add_local(local_kind::var, "undefined", type);
            entry.locals[index_of(local)].is_undefined = true;
            add_stmt({.file = file(), .expr = id}, flat_var{.local = local});
            return local_ref(local, id);
        }
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
            if ((spelling == "==" || spelling == "!=") && arguments.size() == 2
                && tables().type_at(arguments[0].value) == checked_module::void_type
                && tables().type_at(arguments[1].value) == checked_module::void_type)
                return flatten_void_equality(id, type, spelling == "==", arguments[0].value, arguments[1].value);
        }

        for (auto const& t : c.out.ray_traces)
            if (t.call == id && t.file == file())
                return flatten_ray_trace(id, call, t);
        for (auto const& t : c.out.callable_calls)
            if (t.call == id && t.file == file())
                return flatten_callable_call(id, call, t);

        auto const record = tables().call_at(id);
        // CHK-319: a call through a parameter of function type records no callee, and inlines what was handed over
        if (record >= 0 && !is_valid(c.out.call_records[record].callee) && ast::is_valid(call.callee))
        {
            auto const through = tables().target_at(call.callee);
            for (auto const& b : current()->bound)
                if (b.where == through && b.closure >= 0)
                    return flatten_closure_call(id, type, b.closure, c.out.call_records[record]);
            return fail();
        }
        if (record < 0 || (where.kind != target_kind::overload && where.kind != target_kind::constructor))
            return fail();
        return flatten_bound_call(id, type, c.out.call_records[record]);
    }

    /// CHK-329: `trace(world, r, set.ray, mut p)` is the target's trace, with the ray's position in its set as its
    /// contribution and its miss, and the set's size as its multiplier.
    /// The payload is handed over as a local the call writes through, copied in and out where it is another place.
    flat_expr_id flatten_ray_trace(ast::expr_id id, ast::call const& call, ray_trace const& t)
    {
        auto const arguments = ast().at(call.arguments);
        auto const where = origin{.file = file(), .expr = id};
        auto const world = flatten_expr(arguments[0].value);
        auto const world_type = tables().type_at(arguments[0].value);
        auto const* const overloads = c.prelude_names.get_ptr("trace_ray");
        auto callee = symbol_id::none;
        for (auto const candidate :
             overloads == nullptr ? cc::span<symbol_id const>() : cc::span<symbol_id const>(*overloads))
            if (c.out.at(c.out.functions[c.out.at(candidate).info].parameters)[0].type == world_type)
                callee = candidate;
        if (!is_valid(callee) || !is_valid(world))
            return fail();

        // the ray once, then its fields
        auto ray = flatten_expr(arguments[1].value);
        if (!is_valid(ray))
            return fail();
        if (!is_substitutable(ray))
        {
            auto const local = add_local(local_kind::let, "ray", entry.at(ray).type);
            add_stmt(where, flat_let{.local = local, .value = ray});
            ray = local_ref(local, id);
        }
        auto const fields = c.out.at(c.out.at(entry.at(ray).type).members);
        auto const field
            = [&](i32 m) { return add_expr(fields[m].type, id, flat_member{.object = again(ray, id), .member = m}); };

        auto flags = add_expr(c.out.at(c.out.functions[c.out.at(callee).info].parameters)[5].type, id,
                              flat_int_literal{.value = 0});
        auto mask = add_expr(int_type(), id, flat_int_literal{.value = 0xff});
        for (auto i = isize(4); i < arguments.size(); ++i)
        {
            auto const name = c.text_of(file(), arguments[i].name);
            (name == "flags" ? flags : mask) = flatten_expr(arguments[i].value);
        }
        auto const count = c.out.at(c.out.at(c.out.at(t.set).type).members).size();

        auto place = flatten_expr(arguments[3].value);
        if (!is_valid(place))
            return fail();
        auto const is_local = entry.at(place).node.is<flat_local_ref>();
        auto payload = place;
        if (!is_local)
        {
            place = with_bound_index(place, id);
            auto const local = add_local(local_kind::var, "payload", entry.at(place).type);
            add_stmt(where, flat_var{.local = local});
            add_stmt(where, flat_assign{.place = add_expr(entry.at(place).type, id, flat_local_ref{.local = local}),
                                        .value = again(place, id)});
            payload = local_ref(local, id);
        }

        flat_expr_id const values[] = {
            world,
            field(0),
            field(1),
            field(2),
            field(3),
            flags,
            mask,
            add_expr(int_type(), id, flat_int_literal{.value = t.ray}),
            add_expr(int_type(), id, flat_int_literal{.value = i32(count)}),
            add_expr(int_type(), id, flat_int_literal{.value = t.ray}),
            payload,
        };
        auto const traced_ray = flat_traced_ray{.set = t.set, .ray = t.ray};
        if (!entry.traced_rays.values().any([&](flat_traced_ray const& r) { return r == traced_ray; }))
            entry.traced_rays.push_back(traced_ray);
        auto const traced = builtin_call(id, callee, values);
        if (is_local)
            return traced;
        add_stmt(where, flat_eval{.value = traced});
        add_stmt(where, flat_assign{.place = again(place, id), .value = again(payload, id)});
        return add_expr(checked_module::void_type, id, flat_construct{});
    }

    /// CHK-345: a report beyond the ray's range is none, and one in it is what the any hit decides of it.
    flat_expr_id decide_report(origin from, flat_expr_id reported)
    {
        auto const type = entry.at(reported).type;
        auto const id = from.expr;
        auto const members = c.out.at(c.out.at(type).members);
        auto member_at = [&](cc::string_view name)
        {
            for (auto i = isize(0); i < members.size(); ++i)
                if (members[i].name == name)
                    return i32(i);
            return i32(-1);
        };
        auto const is_hit = member_at("is_hit");
        auto const t = member_at("t");
        auto const* const t_min = c.prelude_names.get_ptr("ray_t_min");
        auto const* const t_current = c.prelude_names.get_ptr("ray_t_current");
        auto const* const candidate_of = c.prelude_names.get_ptr("candidate_of_box");
        if (is_hit < 0 || t < 0 || t_min == nullptr || t_current == nullptr || candidate_of == nullptr)
            return fail();
        type_id const floats[] = {c.prelude_type(builtins::k_float), c.prelude_type(builtins::k_float)};
        auto const at_least = c.find_operator(file(), ">=", floats);
        auto const at_most = c.find_operator(file(), "<=", floats);
        if (!is_valid(at_least) || !is_valid(at_most))
            return fail();

        auto const r = add_local(local_kind::var, "reported", type);
        add_stmt(from, flat_var{.local = r, .value = reported});
        auto const field = [&](i32 m)
        { return add_expr(members[m].type, id, flat_member{.object = local_ref(r, id), .member = m}); };
        auto const no_hit = [&]
        {
            return make_stmt(
                from, flat_assign{.place = field(is_hit),
                                  .value = add_expr(members[is_hit].type, id, flat_bool_literal{.value = false})});
        };

        flat_expr_id const low[] = {field(t), builtin_call(id, t_min->front(), {})};
        flat_expr_id const high[] = {field(t), builtin_call(id, t_current->front(), {})};
        auto const in_range
            = add_expr(members[is_hit].type, id,
                       flat_and{.lhs = builtin_call(id, at_least, low), .rhs = builtin_call(id, at_most, high)});

        auto const outer = cc::move(block);
        block = {};
        if (is_valid(fused.value().any_hit))
        {
            // the candidate, as raytracing.sgl builds it from the box and the report
            auto const attributes = c.out.at(type).element;
            auto const builder = candidate_of->front();
            type_id const bindings[] = {c.out.at(c.out.functions[c.out.at(builder).info].type_parameters)[0], attributes};
            flat_expr_id const box_and_report[] = {local_ref(fused.value().box, id), local_ref(r, id)};
            i32 const both[] = {0, 1};
            auto const made = inline_bound(id, builder, box_and_report, both, {}, bindings);
            auto const& info = c.out.functions[c.out.at(builder).info];
            auto const candidate_type = c.substitute_existing(info.result, bindings);
            auto const candidate = add_expr(candidate_type, id, flat_block{.label = made.label, .body = made.body});

            flat_expr_id const handed[] = {candidate, local_ref(fused.value().payload, id)};
            auto const decided = inline_bound(id, fused.value().any_hit, handed, both);
            auto const decision_type = c.out.functions[c.out.at(fused.value().any_hit).info].result;
            auto const decision = add_expr(decision_type, id, flat_block{.label = decided.label, .body = decided.body});
            // `ignore` is the decision's case 1, which no report survives
            type_id const ints[] = {decision_type, decision_type};
            auto const equals = c.find_operator(file(), "==", ints);
            if (!is_valid(equals))
                return fail();
            flat_expr_id const compared[] = {decision, add_expr(decision_type, id, flat_enum_value{.case_index = 1})};
            flat_stmt_id const ignored[] = {no_hit()};
            add_stmt(from, flat_if{.condition = builtin_call(id, equals, compared), .then_body = add_list(ignored)});
        }
        auto const decide = add_list(block);
        block = cc::move(outer);
        flat_stmt_id const out_of_range[] = {no_hit()};
        flat_stmt_id const checked[]
            = {make_stmt(from, flat_if{.condition = in_range, .then_body = decide, .else_body = add_list(out_of_range)})};
        add_stmt(from, flat_if{.condition = field(is_hit), .then_body = add_list(checked)});
        return local_ref(r, id);
    }

    /// CHK-344: `table[i](mut p)` is a call of callable `base + i` of the pipeline's section, where `base` is where the
    /// module's tables before this one end: they pack in declaration order.
    flat_expr_id flatten_callable_call(ast::expr_id id, ast::call const& call, callable_call const& t)
    {
        auto const where = origin{.file = file(), .expr = id};
        auto base = 0;
        for (auto const& p : c.out.pipelines)
            if (p.kind == pipeline_kind::callables && index_of(p.symbol) < index_of(t.table))
                base += i32(p.records.count);
        auto const* const found = c.prelude_names.get_ptr("call_callable");
        if (found == nullptr || found->empty())
            return fail();
        auto const callee = found->front();

        auto const& index_node = ast().at(call.callee).node.as<ast::index>();
        auto index = flatten_expr(ast().at(index_node.arguments)[0].value);
        if (!is_valid(index))
            return fail();
        if (base != 0)
        {
            type_id const types[] = {int_type(), int_type()};
            auto const plus = c.find_operator(file(), "+", types);
            if (!is_valid(plus))
                return fail();
            flat_expr_id const operands[] = {index, add_expr(int_type(), id, flat_int_literal{.value = base})};
            index = builtin_call(id, plus, operands);
        }

        auto place = flatten_expr(ast().at(call.arguments)[0].value);
        if (!is_valid(place))
            return fail();
        auto const is_local = entry.at(place).node.is<flat_local_ref>();
        auto parameter = place;
        if (!is_local)
        {
            place = with_bound_index(place, id);
            auto const local = add_local(local_kind::var, "parameter", entry.at(place).type);
            add_stmt(where, flat_var{.local = local});
            add_stmt(where, flat_assign{.place = add_expr(entry.at(place).type, id, flat_local_ref{.local = local}),
                                        .value = again(place, id)});
            parameter = local_ref(local, id);
        }
        flat_expr_id const values[] = {index, parameter};
        auto const called = builtin_call(id, callee, values);
        if (is_local)
            return called;
        add_stmt(where, flat_eval{.value = called});
        add_stmt(where, flat_assign{.place = again(place, id), .value = again(parameter, id)});
        return add_expr(checked_module::void_type, id, flat_construct{});
    }

    /// The element `place` names, over a local holding its index where evaluating the index twice could differ.
    flat_expr_id with_bound_index(flat_expr_id place, ast::expr_id id)
    {
        // by value: binding adds nodes, and the arrays move
        auto const x = entry.at(place);
        auto bind = [&](flat_expr_id index)
        { return bound_index(index, local_kind::temporary, "index", {.file = file(), .expr = id}); };
        if (auto const* const element = x.node.try_as<flat_buffer_element>())
        {
            auto const buffer = element->buffer;
            auto const index = bind(element->index);
            return add_expr(x.type, id, flat_buffer_element{.buffer = buffer, .index = index});
        }
        if (auto const* const element = x.node.try_as<flat_element>())
        {
            auto const object = element->object;
            auto const index = bind(element->index);
            return add_expr(x.type, id, flat_element{.object = object, .index = index});
        }
        return place;
    }

    /// True where a call of a program function is inlined rather than written as a call of the target.
    [[nodiscard]] bool is_inlined(symbol_id callee) const
    {
        auto const& s = c.out.at(callee);
        return s.role != function_role::constructor && !is_valid(s.intrinsic);
    }

    /// A call as its record says: the written arguments evaluated in the order written, each filling its parameter.
    flat_expr_id flatten_bound_call(ast::expr_id id, type_id type, call_record const& record)
    {
        if (is_by_target(record.callee))
            return flatten_by_target(id, type, record);
        if (auto const texel = atomic_texel_of(record); ast::is_valid(texel))
            return flatten_texel_atomic(id, type, record, texel);
        auto const values = flatten_written(c.out.at(record.written));
        auto const handed = written_closures;
        auto const slots = c.out.at(record.slots);
        if (is_inlined(record.callee))
        {
            auto const inlined = inline_bound(id, record.callee, values, slots, handed, c.out.at(record.type_arguments));
            return add_expr(type, id, flat_block{.label = inlined.label, .body = inlined.body});
        }
        return target_call(id, type, record.callee, values, slots);
    }

    /// The subscript `img[xy]` of an `@atomic` image a call updates as its first argument; none for any other call.
    [[nodiscard]] ast::expr_id atomic_texel_of(call_record const& record) const
    {
        auto const written = c.out.at(record.written);
        if (written.empty())
            return ast::expr_id::none;
        auto const* const indexed = ast().at(written[0].expr).node.try_as<ast::index>();
        if (indexed == nullptr)
            return ast::expr_id::none;
        auto const image = tables().type_at(indexed->object);
        return is_valid(image) && c.out.at(image).kind == type_kind::image && c.out.at(image).is_atomic
                 ? written[0].expr
                 : ast::expr_id::none;
    }

    /// CHK-373: `img[xy].max(v)` is the texel's own atomic, which takes the image, its coordinates and the operands.
    /// The coordinates are those of the plain image's `load`, which the check pass resolved at the subscript.
    flat_expr_id flatten_texel_atomic(ast::expr_id id, type_id type, call_record const& record, ast::expr_id texel)
    {
        auto const* const atomic = c.out.builtin_function(c.out.at(record.callee).intrinsic);
        auto const located = tables().call_at(texel);
        if (atomic == nullptr || located < 0)
            return fail();
        auto const& image = c.out.at(tables().type_at(ast().at(texel).node.as<ast::index>().object));
        auto const callee = texel_atomic(cc::format("texel_{}", atomic->name), image);
        if (!is_valid(callee))
            return fail();
        // the internal atomic takes what the image's `load` does, in the same order, and the operands after it
        auto const& load = c.out.call_records[located];
        auto values = flatten_written(c.out.at(load.written));
        auto slots = cc::vector<i32>::create_copy_of(c.out.at(load.slots));
        auto const written = c.out.at(record.written);
        for (auto i = isize(1); i < written.size(); ++i)
        {
            slots.push_back(i32(values.size()));
            values.push_back(flatten_expr(written[i].expr));
        }
        return target_call(id, type, callee, values, slots);
    }

    /// The prelude's internal atomic `name` of a texel of `image`, by the image's shape and texel.
    [[nodiscard]] symbol_id texel_atomic(cc::string_view name, check::type_info const& image) const
    {
        auto const* const found = c.prelude_names.get_ptr(cc::string(name));
        if (found == nullptr)
            return symbol_id::none;
        auto const texel = texel_name_of(image.format);
        for (auto const f : *found)
        {
            auto const& s = c.out.at(f);
            if (s.kind != symbol_kind::function || s.info < 0)
                continue;
            auto const parameters = c.out.at(c.out.functions[s.info].parameters);
            if (parameters.empty())
                continue;
            auto const& p = c.out.at(parameters[0].type);
            if (p.kind == type_kind::image && p.shape == image.shape && c.out.name_of(p.element) == texel)
                return f;
        }
        return symbol_id::none;
    }

    /// A builtin or a construction over `values`, written in the order the call wrote them.
    /// Where the parameters take them in that order and no two have an effect, they stand in the call itself.
    /// Otherwise each is bound by a `let` in the order written, since a target may evaluate a call's arguments in any
    /// order (EVAL-82), and the call reads the locals.
    flat_expr_id target_call(ast::expr_id id,
                             type_id type,
                             symbol_id callee,
                             cc::span<flat_expr_id const> values,
                             cc::span<i32 const> slots)
    {
        auto const& s = c.out.at(callee);
        auto in_order = slots.size() == values.size();
        for (auto p = isize(0); in_order && p < slots.size(); ++p)
            in_order = slots[p] == p;
        auto with_effect = 0;
        for (auto const v : values)
            with_effect += calls_impure(v, 0) ? 1 : 0;

        auto const call_of = [&](cc::span<flat_expr_id const> arguments)
        {
            if (s.role == function_role::constructor)
                return add_expr(type, id, flat_construct{.arguments = add_list(arguments)});
            return builtin_call(id, callee, arguments);
        };
        if (in_order && with_effect < 2)
            return call_of(values);

        auto const parameters = c.out.at(c.out.functions[s.info].parameters);
        auto const where = origin{.file = file(), .expr = id};
        auto const label = add_label(s.name);
        auto const outer = cc::move(block);
        block = {};
        auto bound = cc::vector<flat_expr_id>::create_filled(values.size(), flat_expr_id::none);
        for (auto i = isize(0); i < values.size(); ++i)
        {
            auto const value = values[i];
            if (!is_valid(value))
                return fail();
            if (is_substitutable(value) || is_resource_member(value) || is_literal_construction(value))
            {
                bound[i] = value;
                continue;
            }
            // an atomic names the memory the call updates, and a binding array's element the resource it hands over,
            // so either's index is bound in its stead, and never its value
            if (auto const kind = c.out.at(entry.at(value).type).kind; kind == type_kind::atomic || is_resource(kind))
            {
                bound[i] = with_bound_index(value, id);
                continue;
            }
            auto name = cc::string_view("argument");
            for (auto p = isize(0); p < slots.size(); ++p)
                if (slots[p] == i)
                    name = parameters[p].name;
            auto const local = add_local(local_kind::let, name, entry.at(value).type);
            add_stmt(where, flat_let{.local = local, .value = value});
            bound[i] = local_ref(local, id);
        }
        // The defaults are written in the callee's frame, which reads its parameters as the call filled them.
        auto has_default = false;
        for (auto const slot : slots)
            has_default = has_default || slot < 0;
        auto filled = cc::vector<bound_name>();
        if (has_default)
        {
            // the copies are made in the caller's frame, where `id` stands, before the callee's frame is pushed
            auto copies = cc::vector<flat_expr_id>();
            for (auto p = isize(0); p < slots.size(); ++p)
                copies.push_back(slots[p] >= 0 ? again(bound[slots[p]], id) : flat_expr_id::none);
            auto const chain = chain_through(id);
            frames.push_back({.function = callee, .file = s.file, .chain = chain});
            for (auto p = isize(0); p < slots.size(); ++p)
                if (slots[p] >= 0)
                    bind_parameter(parameters[p], copies[p], true);
            bind_defaults(parameters, slots);
            filled = cc::move(current()->bound);
            frames.remove_back();
        }

        auto arguments = cc::vector<flat_expr_id>();
        for (auto p = isize(0); p < slots.size(); ++p)
        {
            if (slots[p] >= 0)
            {
                auto const value = bound[slots[p]];
                arguments.push_back(entry.at(value).node.is<flat_local_ref>() || p == 0 ? value : again(value, id));
                continue;
            }
            auto const where_p = target{.kind = target_kind::parameter, .index = i32(parameters[p].field)};
            auto value = flat_expr_id::none;
            for (auto const& b : filled)
                if (b.where == where_p)
                    value = is_valid(b.literal) ? again(b.literal, id) : local_ref(b.local, id);
            if (!is_valid(value))
                return fail();
            arguments.push_back(value);
        }
        // CHK-303: a stream's `emit` takes the vertex past its signature, which no slot names
        if (auto const* const record = c.out.builtin_function(s.intrinsic); record != nullptr && record->takes_element)
            for (auto i = slots.size(); i < bound.size(); ++i)
                arguments.push_back(entry.at(bound[i]).node.is<flat_local_ref>() ? bound[i] : again(bound[i], id));
        add_stmt(where, flat_leave{.target = label, .value = call_of(arguments)});
        auto const body = add_list(block);
        block = cc::move(outer);
        return add_expr(type, id, flat_block{.label = label, .body = body});
    }

    /// True where evaluating `id` has an effect, an inlined call's body included.
    /// A block whose only leave is its last statement leaves its value where it stood (LEGAL-15).
    /// So an inlined `fun f() => counter.add(1)` is an impure call in the text, written wherever the target puts it.
    [[nodiscard]] bool calls_impure(flat_expr_id id, int depth) const
    {
        if (!is_valid(id) || depth > k_max_inline_depth)
            return false;
        auto const& x = entry.at(id);
        if (auto const* const call = x.node.try_as<flat_call>(); call != nullptr && !call->is_pure)
            return true;
        if (auto const* const block = x.node.try_as<flat_block>())
            return writes_outside(entry, block->body, depth + 1);
        auto result = false;
        for_each_operand(entry, x, [&](flat_expr_id operand) { result = result || calls_impure(operand, depth + 1); });
        return result;
    }

    /// `a == b` over void: both sides run for their effects, in order, and the answer is known before either does.
    flat_expr_id flatten_void_equality(ast::expr_id id, type_id type, bool is_equal, ast::expr_id lhs, ast::expr_id rhs)
    {
        auto const label = add_label("void_equality");
        auto const where = origin{.file = file(), .expr = id};
        auto const left = flatten_expr(lhs);
        auto const right = flatten_expr(rhs);
        flat_stmt_id const body[] = {
            make_stmt(where, flat_eval{.value = left}),
            make_stmt(where, flat_eval{.value = right}),
            make_stmt(where,
                      flat_leave{.target = label, .value = add_expr(type, id, flat_bool_literal{.value = is_equal})}),
        };
        return add_expr(type, id, flat_block{.label = label, .body = add_list(body)});
    }

    flat_expr_id builtin_call(ast::expr_id id, symbol_id callee, cc::span<flat_expr_id const> arguments)
    {
        judge_stage(id, callee);
        auto const& s = c.out.at(callee);
        if (!is_valid(s.intrinsic) || s.info < 0)
            return fail();
        auto const& info = c.out.functions[s.info];
        auto const* const record = c.out.builtin_function(s.intrinsic);
        if (record != nullptr && record->with_default_sampler != builtin_id::none)
            return default_sampled_call(id, record->with_default_sampler, arguments);
        if (record != nullptr && record->takes_acceleration_index)
        {
            // CHK-325: the position of the member among the entry point's acceleration members, counted in list order
            auto const* const member = !arguments.empty() && is_valid(arguments[0])
                                         ? entry.at(arguments[0]).node.try_as<flat_binding_member>()
                                         : nullptr;
            if (member == nullptr)
                return fail();
            auto k = 0;
            auto is_found = false;
            for (auto const binding : entry.bindings)
            {
                auto const members = c.out.at(c.out.bindings[c.out.at(binding).info].members);
                for (auto i = isize(0); i < members.size() && !is_found; ++i)
                {
                    if (binding == member->binding && i32(i) == member->member)
                        is_found = true;
                    else if (c.out.at(members[i].type).kind == type_kind::acceleration_structure)
                        ++k;
                }
                if (is_found)
                    break;
            }
            if (!is_found)
                return fail();
            auto widened = cc::vector<flat_expr_id>::create_copy_of(arguments);
            widened.push_back(add_expr(int_type(), id, flat_int_literal{.value = k}));
            return add_expr(info.result, id,
                            flat_call{.callee = callee,
                                      .intrinsic = s.intrinsic,
                                      .is_pure = info.is_pure,
                                      .arguments = add_list(widened)});
        }
        return add_expr(info.result, id,
                        flat_call{.callee = callee,
                                  .intrinsic = s.intrinsic,
                                  .is_pure = info.is_pure,
                                  .arguments = add_list(arguments)});
    }

    /// A sampling call without its sampler calls the record that takes one, with the texture's `@sampler` after the
    /// coordinate (CHK-279): a member of its binding, or a file-scope sampler.
    /// The check pass has made sure the texture names one.
    flat_expr_id default_sampled_call(ast::expr_id id, builtin_id with_sampler, cc::span<flat_expr_id const> arguments)
    {
        // the texture, or the binding array it is an element of
        auto named = arguments.size() >= 2 ? arguments[0] : flat_expr_id::none;
        if (auto const* const element = is_valid(named) ? entry.at(named).node.try_as<flat_element>() : nullptr)
            named = element->object;
        auto const* const texture = is_valid(named) ? entry.at(named).node.try_as<flat_binding_member>() : nullptr;
        if (texture == nullptr)
            return fail();
        auto const members = c.out.at(c.out.bindings[c.out.at(texture->binding).info].members);
        auto const& m = members[texture->member];
        auto const file_sampler = m.default_file_sampler;
        if (m.default_sampler < 0 && (!is_valid(file_sampler) || c.out.at(file_sampler).state != symbol_state::checked))
            return fail();
        // the call becomes one of the record that takes the sampler, so its arguments match its callee's parameters
        auto const declared = c.symbol_declaring(with_sampler);
        if (!is_valid(declared))
            return fail();
        auto with = cc::vector<flat_expr_id>();
        with.push_back_range(arguments);
        with.insert_at(2, is_valid(file_sampler)
                              ? add_expr(c.out.at(file_sampler).type, id, flat_file_sampler{.sampler = file_sampler})
                              : add_expr(members[m.default_sampler].type, id,
                                         flat_binding_member{.binding = texture->binding, .member = m.default_sampler}));
        auto const& info = c.out.functions[c.out.at(declared).info];
        return add_expr(
            info.result, id,
            flat_call{.callee = declared, .intrinsic = with_sampler, .is_pure = info.is_pure, .arguments = add_list(with)});
    }

    /// The values of a call's written arguments, in the order written.
    /// A splat stands for one member access per field, and its value is evaluated once: where the first one stands.
    cc::vector<flat_expr_id> flatten_written(cc::span<written_argument const> written)
    {
        auto result = cc::vector<flat_expr_id>();
        auto handed = cc::vector<i32>();
        auto splat = evaluated_once{};
        auto splat_letters = swizzle();
        for (auto const& w : written)
        {
            handed.push_back(w.is_function ? closure_of(w.expr) : -1);
            if (w.is_function)
            {
                // a function is no value: it is handed over as a closure, and the slot holds nothing
                result.push_back(flat_expr_id::none);
                continue;
            }
            if (w.splat_member < 0)
            {
                result.push_back(flatten_expr(w.expr));
                continue;
            }
            if (w.splat_member == 0)
            {
                // EMIT-142: a splat of a swizzle reads its operand's fields, and binds no vector of them
                splat_letters = {};
                auto read = w.expr;
                if (is_swizzled(w.expr))
                {
                    auto const s = swizzled_at(w.expr);
                    read = s.operand;
                    splat_letters = s.letters;
                }
                auto const value = flatten_expr(read);
                auto const is_local = is_valid(value) && entry.at(value).node.is<flat_local_ref>();
                splat = !is_valid(value) || is_local ? evaluated_once{.first = value, .later = value}
                                                     : evaluate_once(value, "splat", w.expr);
            }
            if (!is_valid(splat.first))
            {
                result.push_back(fail());
                continue;
            }
            auto const object = w.splat_member == 0 ? splat.first : again(splat.later, w.expr);
            auto const field = splat_letters.count > 0 ? i32(splat_letters.fields[w.splat_member]) : w.splat_member;
            result.push_back(add_expr(field_type(entry.at(object).type, field), w.expr,
                                      flat_member{.object = object, .member = field}));
        }
        written_closures = cc::move(handed);
        return result;
    }

    /// The closure the function argument `expr` hands over: a lambda, a function's name, or a parameter handed on.
    i32 closure_of(ast::expr_id expr)
    {
        if (ast().at(expr).node.is<ast::lambda>())
        {
            closures.push_back({.file = file(),
                                .lambda = expr,
                                .bound = current()->bound,
                                .chain = current()->chain,
                                .owner = current()->function,
                                .bindings = current()->bindings});
            return i32(closures.size() - 1);
        }
        auto const where = tables().target_at(expr);
        if (where.kind == target_kind::overload)
        {
            closures.push_back({.function = where.symbol});
            return i32(closures.size() - 1);
        }
        for (auto const& b : current()->bound)
            if (b.where == where && b.closure >= 0)
                return b.closure;
        mark_failed();
        return -1;
    }

    /// `f(args)` where `f` is a parameter of function type: the function it was handed, inlined here (CHK-319).
    flat_expr_id flatten_closure_call(ast::expr_id id, type_id type, i32 handed, call_record const& record)
    {
        auto const values = flatten_written(c.out.at(record.written));
        auto const argument_closures = written_closures;
        auto const slots = c.out.at(record.slots);
        return call_closure(id, type, handed, values, slots, argument_closures);
    }

    /// `by_target(native, emulated)` of raytracing.sgl: both lambdas written out, each a block, and one node holding both.
    [[nodiscard]] bool is_by_target(symbol_id callee) const
    {
        auto const& s = c.out.at(callee);
        return s.name == "by_target" && c.is_prelude_file(s.file);
    }

    flat_expr_id flatten_by_target(ast::expr_id id, type_id type, call_record const& record)
    {
        (void)flatten_written(c.out.at(record.written));
        auto const handed = written_closures;
        if (handed.size() != 2 || handed[0] < 0 || handed[1] < 0)
            return fail();
        auto const native = closure_block(id, type, handed[0], "native");
        auto const emulated = closure_block(id, type, handed[1], "emulated");
        if (!is_valid(native) || !is_valid(emulated))
            return fail();
        return add_expr(type, id, flat_by_target{.native = native, .emulated = emulated});
    }

    /// The closure `handed`, called with nothing, as a block that leaves with its value.
    flat_expr_id closure_block(ast::expr_id id, type_id type, i32 handed, cc::string_view name)
    {
        auto const label = add_label(name);
        auto const outer = cc::move(block);
        block = {};
        auto const value = call_closure(id, type, handed, {}, {}, {});
        if (is_valid(value))
            add_stmt({.file = file(), .expr = id}, flat_leave{.target = label, .value = value});
        auto const body = add_list(block);
        block = cc::move(outer);
        if (!is_valid(value))
            return fail();
        return add_expr(type, id, flat_block{.label = label, .body = body});
    }

    /// The closure `handed`, inlined over `values`, which `slots` assigns to its parameters.
    flat_expr_id call_closure(ast::expr_id id,
                              type_id type,
                              i32 handed,
                              cc::span<flat_expr_id const> values,
                              cc::span<i32 const> slots,
                              cc::span<i32 const> argument_closures)
    {
        // by value: flattening below may push closures, and the vector moves
        auto const f = closures[handed];
        // CHK-404: a function handed a candidate or a box is a decision the traversal calls in its loop
        auto const outer_decision = inlined_decision;
        auto decision = outer_decision;
        if (!values.empty() && is_valid(values[0]))
            if (auto const d = decision_of(entry.at(values[0]).type); d != stage::none)
                decision = d;
        if (is_valid(f.function))
        {
            inlined_decision = decision;
            auto const inlined = inline_bound(id, f.function, values, slots, argument_closures);
            inlined_decision = outer_decision;
            return add_expr(type, id, flat_block{.label = inlined.label, .body = inlined.body});
        }
        auto const& lambda_ast = c.ast_of(f.file);
        auto const& l = lambda_ast.at(f.lambda).node.as<ast::lambda>();
        auto const fields = lambda_ast.at(l.parameters);
        auto bound = f.bound;
        auto const lambda_type = c.out.files[f.file].type_at(f.lambda);
        auto const parameter_types = c.out.at(c.out.at(lambda_type).members);
        for (auto p = isize(0); p < fields.size() && p < slots.size(); ++p)
        {
            auto const k = slots[p];
            auto const field_index = i32(&fields[p] - lambda_ast.fields.data());
            auto const where = target{.kind = target_kind::parameter, .index = field_index};
            if (k < 0 || k >= values.size())
                return fail();
            if (c.out.at(parameter_types[p].type).kind == type_kind::function)
            {
                bound.push_back({.where = where, .closure = argument_closures[k]});
                continue;
            }
            auto const argument = values[k];
            if (!is_valid(argument))
                return fail();
            auto const* const ref = entry.at(argument).node.try_as<flat_local_ref>();
            if (ref != nullptr && is_substitutable(argument))
                bound.push_back({.where = where, .local = ref->local});
            else if (is_substitutable(argument))
                bound.push_back({.where = where, .literal = argument});
            else
            {
                auto const local = add_local(local_kind::let, c.text_of(f.file, fields[p].name), entry.at(argument).type);
                add_stmt(entry.at(argument).from, flat_let{.local = local, .value = argument});
                bound.push_back({.where = where, .local = local});
            }
        }
        frames.push_back({.function = f.owner,
                          .file = f.file,
                          .result = type,
                          .chain = f.chain,
                          .bound = cc::move(bound),
                          .bindings = f.bindings});
        inlined_decision = decision;
        auto const value = flatten_expr(l.body.value);
        inlined_decision = outer_decision;
        frames.remove_back();
        return value;
    }

    /// `any_hit` for a candidate, `intersection` for a box: what a decision of that parameter is to the traversal.
    [[nodiscard]] stage decision_of(type_id type) const
    {
        if (!is_valid(type) || type == checked_module::error_type || c.out.at(type).kind != type_kind::structure)
            return stage::none;
        auto const& s = c.out.at(c.out.at(type).symbol);
        if (!c.is_prelude_file(s.file))
            return stage::none;
        if (s.name == "triangle_candidate" || s.name == "procedural_candidate")
            return stage::any_hit;
        return s.name == "procedural_box" ? stage::intersection : stage::none;
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
            auto const callee = c.find_operator(file(), spelling, types);
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
            auto const body = flatten_arm_body(arm.result, value_block);
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
        auto const compared = c.out.is_plain_enum(type) ? int_type() : type;
        if (!is_valid(compared))
            return symbol_id::none;
        type_id const both[] = {compared, compared};
        return c.find_operator(file(), "==", both);
    }

    /// The prelude's `bool`, even where the user file shadows the name.
    [[nodiscard]] type_id bool_type() const
    {
        auto const* const found = c.prelude_names.get_ptr(builtins::k_bool);
        return found == nullptr || found->empty() ? type_id::none : c.out.at(found->front()).type;
    }

    // ---- checks -----------------------------------------------------------------------------------------------------

    /// A check of a test or an `assert`: every node of `condition` is materialized into a `var` of its own, in the order
    /// and under the conditions the plain expression would run them, and the report reads those `var`s (CHK-229).
    void flatten_check(origin from, ast::expr_id condition, bool stops)
    {
        auto const site = i32(entry.check_sites.size());
        entry.check_sites.push_back({.from = from, .stops = stops});
        auto const outer = cc::move(block);
        block = {};
        auto nodes = cc::vector<flat_check_node>();
        (void)materialize(condition, -1, nodes);
        auto const body = add_list(block);
        block = cc::move(outer);

        // CHK-227: a target writes no assert, so what its condition would write happens on the interpreter alone
        if (stops && writes_outside(entry, body, 0))
        {
            effectful_asserts.push_back({.file = file(), .expr = condition});
            meets_error = true;
            mark_failed();
        }

        auto loop_variables = cc::vector<flat_expr_id>();
        for (auto const index : test_loops)
            loop_variables.push_back(add_expr(entry.at(index).type, ast::expr_id::none, flat_local_ref{.local = index}));
        entry.check_sites[site].nodes = {.first = u32(entry.check_nodes.size()), .count = u32(nodes.size())};
        entry.check_nodes.push_back_range(nodes);
        entry.check_sites[site].loop_variables = add_list(loop_variables);
        add_stmt(from, flat_check{.site = site, .body = body});
    }

    /// A `var` for node `node` of a check, declared without a value where the node's statements begin.
    local_id check_var(ast::expr_id id)
    {
        auto const local = add_local(local_kind::var, "check", tables().type_at(id));
        add_stmt({.file = file(), .expr = id}, flat_var{.local = local});
        return local;
    }

    void assign_check(local_id local, ast::expr_id from, flat_expr_id value)
    {
        add_stmt({.file = file(), .expr = from}, flat_assign{.place = local_ref(local, from), .value = value});
    }

    /// A node that is only a value.
    local_id check_leaf(ast::expr_id id, i32 parent, cc::vector<flat_check_node>& nodes)
    {
        auto const local = check_var(id);
        nodes.push_back(
            {.kind = check_node_kind::leaf, .from = {.file = file(), .expr = id}, .parent = parent, .value = local});
        assign_check(local, id, flatten_expr(id));
        return local;
    }

    /// `callee` called with the values of two nodes, as the comparison the check pass resolved.
    flat_expr_id compare_call(ast::expr_id id, symbol_id callee, local_id lhs, local_id rhs)
    {
        flat_expr_id const arguments[] = {local_ref(lhs, id), local_ref(rhs, id)};
        if (!is_valid(c.out.at(callee).intrinsic))
        {
            auto const inlined = inline_call(id, callee, arguments);
            return add_expr(c.out.functions[c.out.at(callee).info].result, id,
                            flat_block{.label = inlined.label, .body = inlined.body});
        }
        return builtin_call(id, callee, arguments);
    }

    [[nodiscard]] static bool is_comparison(cc::string_view op)
    {
        return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
    }

    local_id materialize(ast::expr_id id, i32 parent, cc::vector<flat_check_node>& nodes)
    {
        auto const& e = ast().at(id);
        auto const* const call = e.node.try_as<ast::call>();
        auto const spelling
            = call != nullptr && sgl::is_valid(call->op) ? c.text_of(file(), c.file_of(file()).at(call->op).where) : "";
        auto const arguments = call != nullptr ? ast().at(call->arguments) : cc::span<ast::argument const>();
        auto const where = tables().target_at(id);
        auto const from = origin{.file = file(), .expr = id};
        auto const index = i32(nodes.size());

        if (spelling == "not" && arguments.size() == 1)
        {
            auto const local = check_var(id);
            nodes.push_back({.kind = check_node_kind::not_, .from = from, .parent = parent, .value = local});
            auto const operand = materialize(arguments[0].value, index, nodes);
            assign_check(local, id, add_expr(tables().type_at(id), id, flat_not{.operand = local_ref(operand, id)}));
            return local;
        }
        if (call != nullptr && call->is_short_circuit && arguments.size() == 2)
        {
            // `a and b`: b's statements run only where a is true, so a skipped operand's `var` holds nothing.
            auto const is_and = spelling == "and";
            auto const local = check_var(id);
            nodes.push_back({.kind = is_and ? check_node_kind::and_ : check_node_kind::or_,
                             .from = from,
                             .parent = parent,
                             .value = local});
            auto const lhs = materialize(arguments[0].value, index, nodes);
            assign_check(local, id, local_ref(lhs, id));
            auto const outer = cc::move(block);
            block = {};
            auto const rhs = materialize(arguments[1].value, index, nodes);
            assign_check(local, id, local_ref(rhs, id));
            auto const then_body = add_list(block);
            block = cc::move(outer);
            auto const decided = is_and ? local_ref(lhs, id)
                                        : add_expr(tables().type_at(id), id, flat_not{.operand = local_ref(lhs, id)});
            add_stmt(from, flat_if{.condition = decided, .then_body = then_body});
            return local;
        }
        if (is_comparison(spelling) && arguments.size() == 2 && where.kind == target_kind::overload)
        {
            auto const local = check_var(id);
            nodes.push_back({.kind = check_node_kind::compare,
                             .from = from,
                             .op = cc::string(spelling),
                             .parent = parent,
                             .value = local});
            auto const lhs = check_leaf(arguments[0].value, index, nodes);
            nodes[index].lhs = i32(nodes.size() - 1);
            auto const rhs = check_leaf(arguments[1].value, index, nodes);
            nodes[index].rhs = i32(nodes.size() - 1);
            assign_check(local, id, compare_call(id, where.symbol, lhs, rhs));
            return local;
        }
        if (auto const* const chain = e.node.try_as<ast::comparison_chain>())
            return materialize_chain(id, *chain, parent, nodes);
        return check_leaf(id, parent, nodes);
    }

    /// `a < b <= c`: each link is a comparison node, and link i + 1 and its new operand run only where link i held.
    local_id materialize_chain(ast::expr_id id,
                               ast::comparison_chain const& chain,
                               i32 parent,
                               cc::vector<flat_check_node>& nodes)
    {
        auto const operands = ast().at(chain.operands);
        auto const operators = ast().at(chain.operators);
        auto const local = check_var(id);
        auto const index = i32(nodes.size());
        nodes.push_back(
            {.kind = check_node_kind::chain, .from = {.file = file(), .expr = id}, .parent = parent, .value = local});
        if (operands.size() < 2 || operators.size() + 1 != operands.size())
        {
            mark_failed();
            return local;
        }

        auto left = check_leaf(operands[0], index, nodes);
        auto left_node = i32(nodes.size() - 1);
        auto outers = cc::vector<cc::vector<flat_stmt_id>>();
        auto conditions = cc::vector<flat_expr_id>();
        for (auto i = isize(0); i < operators.size(); ++i)
        {
            auto const link = check_var(id);
            auto const link_node = i32(nodes.size());
            auto const spelling = c.text_of(file(), c.file_of(file()).at(operators[i]).where);
            nodes.push_back({.kind = check_node_kind::compare,
                             .from = {.file = file(), .expr = id},
                             .op = cc::string(spelling),
                             .parent = index,
                             .lhs = left_node,
                             .value = link});
            auto const right = check_leaf(operands[i + 1], index, nodes);
            nodes[link_node].rhs = i32(nodes.size() - 1);

            type_id const types[] = {entry.at(left).type, entry.at(right).type};
            auto const callee = c.find_operator(file(), spelling, types);
            if (!is_valid(callee))
            {
                mark_failed();
                return local;
            }
            assign_check(link, id, compare_call(id, callee, left, right));
            assign_check(local, id, local_ref(link, id));

            // the next link runs inside an `if` of this one
            if (i + 1 < operators.size())
            {
                conditions.push_back(local_ref(link, id));
                outers.push_back(cc::move(block));
                block = {};
            }
            left = right;
            left_node = nodes[link_node].rhs;
        }
        for (auto i = outers.size() - 1; i >= 0; --i)
        {
            auto const then_body = add_list(block);
            block = cc::move(outers[i]);
            add_stmt({.file = file(), .expr = id}, flat_if{.condition = conditions[i], .then_body = then_body});
        }
        return local;
    }

    /// The prelude's `int`, which an enum's comparison runs on, even where the user file shadows the name.
    type_id int_type() const
    {
        auto const* const found = c.prelude_names.get_ptr(builtins::k_int);
        if (found == nullptr || found->empty() || c.out.at(found->front()).kind != symbol_kind::structure)
            return type_id::none;
        return c.out.at(found->front()).type;
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

    /// The body of a `case` arm or of a branch of an `if` value; `value_block` is what its value leaves, or none.
    ast::range_of<flat_stmt_id> flatten_arm_body(ast::body const& result, label_id value_block)
    {
        auto const outer = cc::move(block);
        block = {};
        if (is_valid(value_block))
            current()->value_blocks.push_back(value_block);

        if (result.kind == ast::body_kind::arrow && ast::is_valid(result.value))
        {
            auto const where = origin{.file = file(), .expr = result.value};
            auto const& e = ast().at(result.value);
            auto const is_jump = e.node.is<ast::return_expr>() || e.node.is<ast::break_expr>()
                              || e.node.is<ast::continue_expr>() || e.node.is<ast::yield_expr>()
                              || e.node.is<ast::discard_expr>();
            if (is_jump)
                flatten_expr_stmt(where, result.value);
            else if (is_valid(value_block))
            {
                auto const value = flatten_expr(result.value);
                add_stmt(where, flat_leave{.target = value_block, .value = value});
            }
            else
                flatten_expr_stmt(where, result.value);
        }
        else
            for (auto const stmt : ast().at(result.statements))
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

    /// `if c => a else b` somebody reads the value of: a block around an `if` whose branches leave it (CHK-375).
    /// Only the branch the condition takes runs.
    /// A branch CHK-356 resolves is the taken side alone, and an arrow side that is a value is its expression.
    flat_expr_id flatten_value_if(ast::expr_id id, type_id type, ast::if_expr const& node)
    {
        auto branches = ast().at(node.branches);
        auto condition = flat_expr_id::none;
        while (branches.size() >= 2 && ast::is_valid(branches.front().condition))
        {
            condition = flatten_expr(branches.front().condition);
            auto const taken = constant_condition(condition);
            if (!taken.has_value())
                break;
            condition = flat_expr_id::none;
            if (taken.value())
                return taken_value(id, type, branches.front().then);
            branches = branches.subspan({.offset = 1, .size = branches.size() - 1});
            if (branches.size() == 1)
                return taken_value(id, type, branches.front().then);
        }
        auto const value_block = add_label("if");
        auto const outer = cc::move(block);
        block = {};
        flatten_if_branches({.file = file(), .expr = id}, branches, value_block, condition);
        auto const body = add_list(block);
        block = cc::move(outer);
        return add_expr(type, id, flat_block{.label = value_block, .body = body});
    }

    /// The value of the side of an `if` value that a constant takes: an arrow's expression, or a block it leaves.
    flat_expr_id taken_value(ast::expr_id id, type_id type, ast::body const& then)
    {
        if (then.kind == ast::body_kind::arrow && ast::is_valid(then.value))
        {
            auto const& e = ast().at(then.value);
            auto const is_jump = e.node.is<ast::return_expr>() || e.node.is<ast::break_expr>()
                              || e.node.is<ast::continue_expr>() || e.node.is<ast::yield_expr>()
                              || e.node.is<ast::discard_expr>();
            if (!is_jump)
                return flatten_expr(then.value);
        }
        auto const value_block = add_label("if");
        auto const body = flatten_arm_body(then, value_block);
        return add_expr(type, id, flat_block{.label = value_block, .body = body});
    }

    /// CHK-356: the value of `condition` where it is a constant, which a branch on it is replaced by the taken side of.
    /// Nothing for a condition that is no constant, and for one with no value, which `judge_constants` reports.
    [[nodiscard]] cc::optional<bool> constant_condition(flat_expr_id condition) const
    {
        if (!is_valid(condition) || !is_constant(c.out, entry, condition))
            return {};
        auto const o = evaluate_constant(c.out, entry, condition);
        if (o.status != run_status::ok || o.result.leaves.size() != 1 || o.result.leaves[0].kind != value_kind::boolean)
            return {};
        return o.result.leaves[0].as_bool();
    }

    /// The taken side of a branch on a constant, as a block of its own, so its locals stay in its scope.
    void add_taken(origin from, ast::range_of<flat_stmt_id> body)
    {
        add_stmt(from, flat_block{.label = add_label("if"), .body = body});
    }

    /// The branches of an `if` value, each an `if` nested in the `else` of the one before; the last has no condition.
    /// `first`, where valid, is the first condition flattened already.
    void flatten_if_branches(origin from,
                             cc::span<ast::if_branch const> branches,
                             label_id value_block,
                             flat_expr_id first = flat_expr_id::none)
    {
        if (branches.size() < 2 || !ast::is_valid(branches.front().condition))
        {
            mark_failed();
            return;
        }
        auto const condition = is_valid(first) ? first : flatten_expr(branches.front().condition);
        auto const rest = branches.subspan({.offset = 1, .size = branches.size() - 1});
        // CHK-356: only the side a constant takes is flattened, so nothing judges what the other side would use
        if (auto const taken = constant_condition(condition); taken.has_value())
        {
            if (taken.value())
                return add_taken(from, flatten_arm_body(branches.front().then, value_block));
            if (rest.size() == 1)
                return add_taken(from, flatten_arm_body(rest.front().then, value_block));
            return flatten_if_branches(from, rest, value_block);
        }
        auto const then_body = flatten_arm_body(branches.front().then, value_block);
        auto else_body = ast::range_of<flat_stmt_id>();
        if (rest.size() == 1)
            else_body = flatten_arm_body(rest.front().then, value_block);
        else
        {
            auto const outer = cc::move(block);
            block = {};
            flatten_if_branches(from, rest, value_block);
            else_body = add_list(block);
            block = cc::move(outer);
        }
        add_stmt(from, flat_if{.condition = condition, .then_body = then_body, .else_body = else_body});
    }

    // ---- calls ------------------------------------------------------------------------------------------------------

    struct inlined_body
    {
        label_id label = label_id::none;
        ast::range_of<flat_stmt_id> body;
    };

    /// The call sites a node inlined through `call` stands under: the current frame's, then `call`.
    ast::range_of<call_site> chain_through(ast::expr_id call)
    {
        // The chain is copied first, since `call_sites` grows under the span that names it.
        auto chain = cc::vector<call_site>();
        chain.push_back_range(entry.at(current()->chain));
        chain.push_back({.file = file(), .call = call});
        auto const range = ast::range_of<call_site>{.first = u32(entry.call_sites.size()), .count = u32(chain.size())};
        entry.call_sites.push_back_range(chain);
        return range;
    }

    /// Binds a value to parameter `p` in the current frame: a literal or an immutable local stands for it, and
    /// anything else is bound once by a `let` of the block being written.
    /// `keeps_constructions` binds a construction of literals as it stands, for a builtin that takes one only so.
    void bind_parameter(parameter const& p, flat_expr_id value, bool keeps_constructions = false)
    {
        if (!is_valid(value))
        {
            mark_failed();
            return;
        }
        auto const where = target{.kind = target_kind::parameter, .index = i32(p.field)};
        auto const& x = entry.at(value);
        auto const* const ref = x.node.try_as<flat_local_ref>();
        if (ref != nullptr && is_substitutable(value))
            current()->bound.push_back({.where = where, .local = ref->local});
        else if (is_substitutable(value) || is_resource_member(value)
                 || (keeps_constructions && is_literal_construction(value)))
            current()->bound.push_back({.where = where, .literal = value});
        else
        {
            auto const local = add_local(local_kind::let, p.name, x.type);
            add_stmt(x.from, flat_let{.local = local, .value = value});
            current()->bound.push_back({.where = where, .local = local});
        }
    }

    /// The default of every parameter `slots` leaves unfilled, flattened in the callee's frame, which is the current
    /// one, and bound in parameter order (EVAL-80, EVAL-81).
    void bind_defaults(cc::span<parameter const> parameters, cc::span<i32 const> slots)
    {
        for (auto p = isize(0); p < parameters.size(); ++p)
        {
            if (slots[p] >= 0)
                continue;
            if (!parameters[p].has_default || !ast::is_valid(parameters[p].field))
            {
                mark_failed();
                continue;
            }
            bind_parameter(parameters[p], flatten_expr(ast().at(parameters[p].field).default_value));
        }
    }

    /// `inline_bound` for arguments that fill the parameters in order, one each.
    inlined_body inline_call(ast::expr_id call, symbol_id callee, cc::span<flat_expr_id const> arguments)
    {
        auto slots = cc::vector<i32>();
        for (auto i = isize(0); i < arguments.size(); ++i)
            slots.push_back(i32(i));
        return inline_bound(call, callee, arguments, slots);
    }

    /// The body of `callee` as the statements of a block named after it.
    /// `values` were written in the caller's frame, in the order the call wrote them, and are bound at the top of the
    /// block in that order; `slots` says which parameter each one fills.
    inlined_body inline_bound(ast::expr_id call,
                              symbol_id callee,
                              cc::span<flat_expr_id const> values,
                              cc::span<i32 const> slots,
                              cc::span<i32 const> handed = {},
                              cc::span<type_id const> type_arguments = {})
    {
        // CHK-340: what the callee's type parameters stand for, in the caller's terms made concrete
        auto bindings = cc::vector<type_id>();
        for (auto i = isize(0); i + 1 < type_arguments.size(); i += 2)
        {
            bindings.push_back(type_arguments[i]);
            bindings.push_back(concrete(type_arguments[i + 1]));
        }
        judge_stage(call, callee);
        auto const& s = c.out.at(callee);
        auto is_open = s.info < 0 || frames.size() > k_max_inline_depth;
        for (auto const& f : frames)
            is_open = is_open || f.function == callee;
        auto const* const node = is_open ? nullptr : &c.ast_of(s.file).at(s.declaration).node;
        auto const* const function = node != nullptr ? node->try_as<ast::fun_decl>() : nullptr;
        auto const* const property = node != nullptr ? node->try_as<ast::property_decl>() : nullptr;
        auto const* const source = function != nullptr ? &function->body
                                 : property != nullptr ? &property->body
                                                       : nullptr;
        // recursion was reported by the check pass, and an entry point that reaches it is never written
        if (source == nullptr)
        {
            mark_failed();
            return {};
        }
        auto const& info = c.out.functions[s.info];
        auto const parameters = c.out.at(info.parameters);
        auto filled = cc::vector<i32>::create_filled(values.size(), -1);
        for (auto p = isize(0); p < slots.size(); ++p)
            if (slots[p] >= 0 && slots[p] < values.size())
                filled[slots[p]] = i32(p);
        auto is_complete = parameters.size() == slots.size();
        for (auto p = isize(0); is_complete && p < slots.size(); ++p)
            is_complete = slots[p] >= 0 || (parameters[p].has_default && ast::is_valid(parameters[p].field));
        for (auto const p : filled)
            is_complete = is_complete && p >= 0;
        if (!is_complete)
        {
            mark_failed();
            return {};
        }

        auto const chain_range = chain_through(call);

        auto const label = add_label(s.name);
        auto const outer = cc::move(block);
        block = {};

        // A parameter is a value: a literal or an immutable local stands for it, and anything else is bound once.
        auto bound = cc::vector<bound_name>();
        for (auto k = isize(0); k < values.size(); ++k)
        {
            auto const i = filled[k];
            auto const where = target{.kind = target_kind::parameter, .index = i32(parameters[i].field)};
            // CHK-319: a parameter of function type stands for the closure its argument handed over
            if (c.out.at(parameters[i].type).kind == type_kind::function)
            {
                if (k >= handed.size() || handed[k] < 0)
                {
                    mark_failed();
                    continue;
                }
                bound.push_back({.where = where, .closure = handed[k]});
                continue;
            }
            auto const argument = values[k];
            if (!is_valid(argument))
            {
                mark_failed();
                continue;
            }
            // CHK-316: a `mut` parameter is the caller's place, named again wherever the body names the parameter
            if (parameters[i].is_mut)
            {
                bound.push_back({.where = where, .literal = pin_place(argument)});
                continue;
            }
            auto const& x = entry.at(argument);
            auto const* const ref = x.node.try_as<flat_local_ref>();
            if (ref != nullptr && is_substitutable(argument))
                bound.push_back({.where = where, .local = ref->local});
            // CHK-324: a resource stands wherever the parameter is named, since no target holds one in a local
            else if (is_substitutable(argument) || is_resource_member(argument))
                bound.push_back({.where = where, .literal = argument});
            // an element of a binding array at an index computed once, named again wherever the parameter is (CHK-366)
            else if (is_resource(c.out.at(x.type).kind) && x.node.is<flat_element>())
                bound.push_back({.where = where, .literal = pin_place(argument)});
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
                          .bound = cc::move(bound),
                          .bindings = cc::move(bindings)});
        // `self`, the first parameter of a method or a property, is the receiver its body and its defaults read
        auto const has_receiver = s.role == function_role::property
                               || (s.role == function_role::method && function->receiver == ast::receiver_kind::self);
        if (has_receiver)
        {
            auto const first = target{.kind = target_kind::parameter, .index = i32(parameters[0].field)};
            for (isize i = 0, n = current()->bound.size(); i < n; ++i)
                if (current()->bound[i].where == first)
                {
                    auto receiver = current()->bound[i];
                    receiver.where = {.kind = target_kind::receiver};
                    current()->bound.push_back(receiver);
                }
        }
        // EVAL-80: the defaults of the parameters the call left out come after every written argument, in parameter
        // order, and each reads the parameters before it.
        bind_defaults(parameters, slots);
        // a property's `=>:` block has its value through `yield`, which leaves the property's block
        if (property != nullptr && !ast::is_valid(source->value))
            current()->value_blocks.push_back(label);
        // an arrow body of no value is a statement, such as `=> img.store(xy, v)`, which a leave would drop
        if (ast::is_valid(source->value) && info.result == checked_module::void_type)
            flatten_expr_stmt({.file = s.file, .expr = source->value}, source->value);
        else if (ast::is_valid(source->value))
            flatten_return({.file = s.file, .expr = source->value}, source->value);
        for (auto const stmt : ast().at(source->statements))
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
        {
            // CHK-389: in a copy, what follows a constant exit is never written, so no rule judges it
            if (unrolled_depth > 0 && !block.empty() && certain_exit(block.back()).has_value())
                break;
            flatten_stmt(stmt);
        }
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
            result = flatten_expr(value);
        // CHK-345: the entry point's own report, decided by the fused any hit before it leaves
        if (fused.has_value() && frames.size() == 1 && is_valid(result))
            result = decide_report(from, result);
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
            mark_failed();
            return;
        }
        auto const condition = flatten_expr(first.condition);
        auto const rest = branches.subspan({.offset = 1, .size = branches.size() - 1});
        // CHK-356: only the side a constant takes is flattened, so nothing judges what the other side would use
        if (auto const taken = constant_condition(condition); taken.has_value())
        {
            if (taken.value())
                return add_taken(from, flatten_body(first.then));
            if (!rest.empty() && !ast::is_valid(rest.front().condition))
                return add_taken(from, flatten_body(rest.front().then));
            return flatten_if(from, rest);
        }
        auto const then_body = flatten_body(first.then);

        auto else_body = ast::range_of<flat_stmt_id>();
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
        for (auto const& t : c.out.texel_stores)
            if (t.place == assign.target && t.file == file())
                return flatten_texel_store(from, assign, t);
        if (is_swizzled(assign.target) && sgl::is_valid(assign.op))
            return flatten_swizzle_assign(from, assign, c.text_of(file(), c.file_of(file()).at(assign.op).where));
        auto const place = flatten_expr(assign.target);
        auto value = flatten_expr(assign.value);
        auto const op = sgl::is_valid(assign.op) ? c.text_of(file(), c.file_of(file()).at(assign.op).where) : "";
        if (!is_valid(place) || !is_valid(value) || op.empty())
        {
            mark_failed();
            return;
        }
        if (op != "=")
        {
            type_id const types[] = {entry.at(place).type, entry.at(value).type};
            auto const callee = c.find_operator(file(), op.subview({.offset = 0, .size = op.size() - 1}), types);
            if (!is_valid(callee))
            {
                mark_failed();
                return;
            }
            flat_expr_id const arguments[] = {read_of_place(place, assign.target), value};
            value = builtin_call(assign.value, callee, arguments);
        }
        judge_decision_store(place, assign.target);
        add_stmt(from, flat_assign{.place = place, .value = value});
    }

    /// CHK-367: `img[xy] = v` is the `store` the check pass resolved, and `img[xy] op= v` loads the texel first.
    /// A compound one evaluates the image's index and the coordinates once, which the load and the store both read.
    void flatten_texel_store(origin from, ast::assign_stmt const& assign, texel_store const& t)
    {
        auto const& store = c.out.call_records[t.store];
        if (t.load < 0)
        {
            add_stmt(from, flat_eval{.value = flatten_bound_call(assign.target, checked_module::void_type, store)});
            return;
        }
        auto const& load = c.out.call_records[t.load];
        auto coordinates = flatten_written(c.out.at(load.written));
        for (auto i = isize(0); i < coordinates.size(); ++i)
        {
            if (!is_valid(coordinates[i]))
            {
                mark_failed();
                return;
            }
            if (i == 0)
            {
                coordinates[i] = with_bound_index(coordinates[i], assign.target);
                continue;
            }
            if (is_substitutable(coordinates[i]))
                continue;
            auto const local = add_local(local_kind::temporary, "coordinate", entry.at(coordinates[i]).type);
            add_stmt(from, flat_let{.local = local, .value = coordinates[i]});
            coordinates[i] = local_ref(local, assign.target);
        }
        auto const texel = tables().type_at(assign.target);
        auto const loaded = target_call(assign.target, texel, load.callee, coordinates, c.out.at(load.slots));
        auto const value = flatten_expr(assign.value);
        auto const op = c.text_of(file(), c.file_of(file()).at(assign.op).where);
        type_id const types[] = {texel, is_valid(value) ? entry.at(value).type : type_id::none};
        auto const operator_callee = c.find_operator(file(), op.subview({.offset = 0, .size = op.size() - 1}), types);
        if (!is_valid(loaded) || !is_valid(value) || !is_valid(operator_callee))
        {
            mark_failed();
            return;
        }
        flat_expr_id const operands[] = {loaded, value};
        auto const combined = builtin_call(assign.value, operator_callee, operands);

        // the store's written arguments are the load's, then the value
        auto values = cc::vector<flat_expr_id>();
        for (auto const coordinate : coordinates)
            values.push_back(again(coordinate, assign.target));
        values.push_back(combined);
        add_stmt(from, flat_eval{.value = target_call(assign.target, checked_module::void_type, store.callee, values,
                                                      c.out.at(store.slots))});
    }

    /// What `place op= value` reads the place as, which is the place read a second time.
    /// A local or a member of one holds nothing to evaluate, so it is simply flattened again.
    /// A buffer element's index is evaluated once (EVAL-14): the place gets its first evaluation, and the read the local
    /// that holds it.
    flat_expr_id read_of_place(flat_expr_id place, ast::expr_id target)
    {
        // a `mut` parameter names a place whose indices were evaluated when it was bound (`pin_place`)
        if (is_valid(place) && ast().at(target).node.is<ast::name>())
            return again(place, target);
        if (has_array_index(place))
            return reread(place, target);
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

    [[nodiscard]] bool has_array_index(flat_expr_id id) const
    {
        for (auto depth = 0; is_valid(id) && depth < k_max_inline_depth; ++depth)
        {
            auto const& node = entry.at(id).node;
            if (node.is<flat_element>())
                return true;
            auto const* const member = node.try_as<flat_member>();
            if (member == nullptr)
                return false;
            id = member->object;
        }
        return false;
    }

    /// The place `id` read a second time, each array index in it evaluated once: the place keeps the first evaluation,
    /// and the read takes the local that holds it (EVAL-14).
    flat_expr_id reread(flat_expr_id id, ast::expr_id from)
    {
        // by value: evaluating once adds nodes, and the arrays move
        auto const x = entry.at(id);
        if (auto const* const member = x.node.try_as<flat_member>())
        {
            auto const index = member->member;
            auto const letters = member->letters;
            auto const object = reread(member->object, from);
            return add_expr(x.type, from, flat_member{.object = object, .member = index, .letters = letters});
        }
        if (auto const* const element = x.node.try_as<flat_element>())
        {
            auto const object = reread(element->object, from);
            auto const once = evaluate_once(element->index, "index", from);
            entry.exprs[index_of(id)].node = flat_element{.object = element->object, .index = once.first};
            auto const index = once.later == once.first ? again(once.first, from) : once.later;
            return add_expr(x.type, from, flat_element{.object = object, .index = index});
        }
        // a local and a binding's member, `@workgroup` memory among them, hold nothing to evaluate
        if (x.node.is<flat_local_ref>() || x.node.is<flat_binding_member>())
            return again(id, from);
        return fail();
    }

    /// The most copies of its body an unrolled `for` writes, each counted once per copy of the unrolled loops around it (CHK-389).
    static constexpr i64 k_max_unrolled = 256;

    /// The value of constant `bound`, an int, where it is one; nothing for anything else.
    /// The innermost call the statement being written was inlined through, none in the tree's own body.
    [[nodiscard]] call_site innermost_call() const
    {
        auto const chain = entry.at(frames.back().chain);
        return chain.empty() ? call_site{} : chain[chain.size() - 1];
    }

    [[nodiscard]] cc::optional<i32> constant_int(flat_expr_id bound) const
    {
        if (!is_valid(bound) || !is_constant(c.out, entry, bound))
            return {};
        auto const o = evaluate_constant(c.out, entry, bound);
        if (o.status != run_status::ok || o.result.leaves.size() != 1 || o.result.leaves[0].kind != value_kind::scalar_int)
            return {};
        return o.result.leaves[0].as_int();
    }

    /// The label a run of `id` certainly leaves to, `none` for a return, where no run of it reaches its end.
    /// Nothing where a run may reach its end, or where two of them leave to different places.
    [[nodiscard]] cc::optional<label_id> certain_exit(flat_stmt_id id) const
    {
        auto const& node = entry.at(id).node;
        if (auto const* const leave = node.try_as<flat_leave>())
            return leave->target;
        if (auto const* const next = node.try_as<flat_continue>())
            return next->target;
        if (node.is<flat_return>())
            return label_id::none;
        if (auto const* const b = node.try_as<flat_block>())
        {
            auto const exit = certain_exit(b->body);
            if (exit.has_value() && exit.value() == b->label)
                return {};
            return exit;
        }
        if (auto const* const branch = node.try_as<flat_if>())
        {
            auto const then_exit = certain_exit(branch->then_body);
            if (then_exit.has_value() && then_exit == certain_exit(branch->else_body))
                return then_exit;
        }
        return {};
    }

    [[nodiscard]] cc::optional<label_id> certain_exit(ast::range_of<flat_stmt_id> body) const
    {
        for (auto const stmt : entry.at(body))
            if (auto const exit = certain_exit(stmt); exit.has_value())
                return exit;
        return {};
    }

    /// CHK-389: `@unroll for i in a ..< b`, as one block holding a block per index, each a copy of the body in which
    /// `i` is that index's literal.
    /// A `break` leaves the outer block and a `continue` the copy it stands in, which is what they do in the loop.
    /// A range whose copies would pass the limit is the loop it would be without the attribute, and a warning says so.
    void flatten_unrolled_for(origin from, ast::stmt_id id, ast::for_stmt const& loop, flat_expr_id first, flat_expr_id end)
    {
        if (!is_valid(first) || !is_valid(end))
        {
            mark_failed();
            return;
        }
        auto const low = constant_int(first);
        auto const high = constant_int(end);
        auto const range_file = file();
        auto const range_where = c.span_of(range_file, loop.iterable);
        if ((!low.has_value() || !high.has_value()) && looped_around.has_value())
        {
            auto const& around = looped_around.value();
            unroll_reports.push_back(
                {.kind = diagnostic_kind::unroll_too_long,
                 .file = range_file,
                 .where = range_where,
                 .detail = cc::format("an unrolled for runs over a constant range, and this one is no constant inside "
                                      "the unrolled for over `{}`, which is written as a loop, so this one is written "
                                      "as a loop too",
                                      c.text_of(around.file, around.where)),
                 .site = innermost_call()});
            return flatten_fallen_back_for(from, id, loop, first, end);
        }
        if (!low.has_value() || !high.has_value())
        {
            unroll_reports.push_back({.file = range_file,
                                      .where = range_where,
                                      .detail = "an unrolled for runs over a constant range: both bounds are int "
                                                "constants once the function is inlined, such as `0 ..< taps` with "
                                                "`taps` a const",
                                      .site = innermost_call()});
            mark_failed();
            meets_error = true;
            return;
        }
        // in 64 bits, since a range may hold more values than an int counts
        auto const count = cc::max(i64(0), i64(high.value()) - i64(low.value()));
        // the copies around it are at most k_max_unrolled, so this cannot overflow either
        auto const copies = count * unrolled_copies;
        if (copies > k_max_unrolled)
        {
            auto const detail
                = unrolled_copies == 1
                    ? cc::format("an unrolled for writes its body at most {} times, and this range holds {}, so it is "
                                 "written as a loop",
                                 k_max_unrolled, count)
                    : cc::format("an unrolled for writes its body at most {} times, counted once per copy of the "
                                 "unrolled loops around it, and this range of {} inside {} copies would write {}, so "
                                 "it is written as a loop",
                                 k_max_unrolled, count, unrolled_copies, copies);
            unroll_reports.push_back({.kind = diagnostic_kind::unroll_too_long,
                                      .file = range_file,
                                      .where = range_where,
                                      .detail = detail,
                                      .site = innermost_call()});
            return flatten_fallen_back_for(from, id, loop, first, end);
        }

        auto const walk = take_walk();
        auto const type = entry.at(first).type;
        auto const* const n = ast::is_valid(loop.variable) ? ast().at(loop.variable).node.try_as<ast::name>() : nullptr;
        auto const index_from = ast::is_valid(loop.variable) ? loop.variable : loop.iterable;
        // every read of the variable is the literal of the copy being written, which a copy rebinds
        auto const slot = current()->bound.size();
        if (n != nullptr)
            current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)}});
        // what a copy binds is its own: a lookup takes the first binding of a name, so the next copy must not see it
        auto const bound_before_body = current()->bound.size();

        auto const label = add_label("unrolled");
        auto const outer = cc::move(block);
        block = {};
        auto const copies_around = unrolled_copies;
        unrolled_copies = copies;
        for (auto i = i64(0); i < count; ++i)
        {
            if (n != nullptr)
            {
                auto const k = add_expr(type, index_from, flat_int_literal{.value = i32(i64(low.value()) + i)});
                current()->bound[slot].literal = walk.has_value() ? walked(walk.value(), k, index_from) : k;
            }
            auto const copy = add_label("unrolled_index");
            current()->loops.push_back({.loop = label, .continue_block = copy});
            ++unrolled_depth;
            auto const body = flatten_body(loop.body);
            --unrolled_depth;
            current()->loops.remove_back();
            current()->bound.resize_down_to(bound_before_body);
            add_stmt(from, flat_block{.label = copy, .body = body});
            // a copy that leaves the loop whatever runs, such as one a constant `break` ends, has no copy after it
            auto const exit = certain_exit(body);
            if (exit.has_value() && exit.value() != copy)
                break;
        }
        unrolled_copies = copies_around;
        auto const written = add_list(block);
        block = cc::move(outer);
        add_stmt(from, flat_block{.label = label, .body = written});
    }

    /// An unrolled `for` written as a loop, which the unrolled `for`s inside it see.
    void flatten_fallen_back_for(origin from, ast::stmt_id id, ast::for_stmt const& loop, flat_expr_id first, flat_expr_id end)
    {
        auto const around = looped_around;
        looped_around = looped_unroll{.file = file(), .where = c.span_of(file(), loop.iterable)};
        flatten_looped_for(from, id, loop, first, end);
        looped_around = around;
    }

    /// `@unroll(false)` on the loop `id`, which stays a loop and asks its target to keep it one (CHK-414).
    [[nodiscard]] bool is_rolled(ast::stmt_id id) const
    {
        auto const* const a = c.find_attribute(file(), ast().at(id).attributes, "unroll");
        if (a == nullptr)
            return false;
        auto const arguments = ast().at(a->arguments);
        return arguments.size() == 1 && ast::is_valid(arguments[0].value)
            && c.text_of(file(), c.span_of(file(), arguments[0].value)) == "false";
    }

    /// `@unroll` on the loop `id`, written without `false` (CHK-389).
    [[nodiscard]] bool is_unrolled(ast::stmt_id id) const
    {
        return c.find_attribute(file(), ast().at(id).attributes, "unroll") != nullptr && !is_rolled(id);
    }

    /// `for i in a ..< b` as a loop of the flat tree, whose bounds `first` and `end` are flattened already.
    void flatten_looped_for(origin from, ast::stmt_id id, ast::for_stmt const& loop, flat_expr_id first, flat_expr_id end)
    {
        auto const* const n = ast::is_valid(loop.variable) ? ast().at(loop.variable).node.try_as<ast::name>() : nullptr;
        auto const int_type = is_valid(first) ? entry.at(first).type : type_id::none;
        auto const walk = take_walk();
        auto const index = add_local(local_kind::index,
                                     walk.has_value() ? cc::string_view("k")
                                     : n != nullptr   ? c.text_of(file(), n->where)
                                                      : cc::string_view("i"),
                                     int_type);
        current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)},
                                    .local = index,
                                    .literal = walk.has_value()
                                                 ? walked(walk.value(), local_ref(index, loop.iterable), loop.iterable)
                                                 : flat_expr_id::none});

        auto const label = add_label("for");
        entry.labels[index_of(label)].is_rolled = is_rolled(id);
        auto const is_test_loop = is_test && frames.size() == 1;
        if (is_test_loop)
            test_loops.push_back(index);
        current()->loops.push_back({.loop = label});
        auto const body = flatten_body(loop.body);
        current()->loops.remove_back();
        if (is_test_loop)
            test_loops.remove_back();
        add_stmt(from, flat_for{.label = label, .index = index, .first = first, .end = end, .body = body});
    }

    /// `for i in a ..= b`: the `for` over `a ..< b + 1` where `b` is a constant below the largest int, and otherwise a
    /// loop that tests after its body, so `b + 1` is never computed and a range up to the largest int runs whole.
    /// A `continue` leaves the body's block and reaches the test, as it reaches the next index of a `for`.
    void flatten_inclusive_for(origin from, ast::stmt_id id, ast::for_stmt const& loop, flat_expr_id first, flat_expr_id last)
    {
        if (!is_valid(first) || !is_valid(last))
        {
            mark_failed();
            return;
        }
        auto const int_type = entry.at(first).type;
        auto const unrolls = is_unrolled(id);
        if (auto const high = constant_int(last); high.has_value() && high.value() < 2147483647)
        {
            auto const end = add_expr(int_type, loop.iterable, flat_int_literal{.value = high.value() + 1});
            if (unrolls)
                return flatten_unrolled_for(from, id, loop, first, end);
            return flatten_looped_for(from, id, loop, first, end);
        }
        type_id const ints[] = {int_type, int_type};
        auto const at_most = c.find_operator(file(), "<=", ints);
        auto const equals = c.find_operator(file(), "==", ints);
        auto const plus = c.find_operator(file(), "+", ints);
        if (!is_valid(at_most) || !is_valid(equals) || !is_valid(plus))
        {
            mark_failed();
            return;
        }
        auto const where = loop.iterable;
        // both bounds once, first before last, as a `for` evaluates them
        auto const low = add_local(local_kind::temporary, "first", int_type);
        add_stmt(from, flat_let{.local = low, .value = first});
        auto const high = add_local(local_kind::temporary, "last", int_type);
        add_stmt(from, flat_let{.local = high, .value = last});

        auto const outer = cc::move(block);
        block = {};
        auto const* const n = ast::is_valid(loop.variable) ? ast().at(loop.variable).node.try_as<ast::name>() : nullptr;
        auto const index
            = add_local(local_kind::var, n != nullptr ? c.text_of(file(), n->where) : cc::string_view("i"), int_type);
        add_stmt(from, flat_var{.local = index, .value = local_ref(low, where)});
        current()->bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)}, .local = index});

        auto const label = add_label("for");
        entry.labels[index_of(label)].is_rolled = is_rolled(id);
        auto const iteration = add_label("iteration");
        auto const is_test_loop = is_test && frames.size() == 1;
        if (is_test_loop)
            test_loops.push_back(index);
        current()->loops.push_back({.loop = label, .continue_block = iteration});
        auto const body = flatten_body(loop.body);
        current()->loops.remove_back();
        if (is_test_loop)
            test_loops.remove_back();

        auto const around = cc::move(block);
        block = {};
        add_stmt(from, flat_block{.label = iteration, .body = body});
        flat_expr_id const reached[] = {local_ref(index, where), local_ref(high, where)};
        flat_stmt_id const leave[] = {make_stmt(from, flat_leave{.target = label})};
        add_stmt(from, flat_if{.condition = builtin_call(where, equals, reached), .then_body = add_list(leave)});
        flat_expr_id const next[] = {local_ref(index, where), add_expr(int_type, where, flat_int_literal{.value = 1})};
        add_stmt(from, flat_assign{.place = local_ref(index, where), .value = builtin_call(where, plus, next)});
        auto const steps = add_list(block);
        block = cc::move(around);
        add_stmt(from, flat_loop{.label = label, .body = steps});

        auto const runs = add_list(block);
        block = cc::move(outer);
        flat_expr_id const ordered[] = {local_ref(low, where), local_ref(high, where)};
        add_stmt(from, flat_if{.condition = builtin_call(where, at_most, ordered), .then_body = runs});
    }

    /// The walk set for the `for` being flattened, which no `for` inside its body sees.
    [[nodiscard]] cc::optional<walk_mapping> take_walk()
    {
        auto const walk = pending_walk;
        pending_walk = {};
        return walk;
    }

    /// `start + k * step`, folded where its parts are constants, so a walk over a constant range reads as one.
    flat_expr_id walked(walk_mapping const& walk, flat_expr_id k, ast::expr_id where)
    {
        auto const type = entry.at(k).type;
        auto const start = constant_int(walk.start);
        auto const index = constant_int(k);
        auto const literal = [&](i32 v) { return add_expr(type, where, flat_int_literal{.value = v}); };
        if (start.has_value() && index.has_value())
            return literal(start.value() + index.value() * walk.step);
        type_id const ints[] = {type, type};
        auto const op = [&](cc::string_view spelling, flat_expr_id a, flat_expr_id b)
        {
            flat_expr_id const operands[] = {a, b};
            return builtin_call(where, c.find_operator(file(), spelling, ints), operands);
        };
        auto const is_zero = start.has_value() && start.value() == 0;
        if (walk.step == -1)
            return op("-", is_zero ? literal(0) : again(walk.start, where), again(k, where));
        auto const scaled = walk.step == 1 ? again(k, where) : op("*", again(k, where), literal(walk.step));
        return is_zero ? scaled : op("+", again(walk.start, where), scaled);
    }

    /// CHK-415: a range walked in reverse or in steps, as a `for` of `k` over `0 ..< count` whose variable is the
    /// walk's `k`-th value; every part is an int, its bounds evaluated once, first before last.
    void flatten_walked_for(origin from, ast::stmt_id id, ast::for_stmt const& loop, checker::range_walk const& walk)
    {
        auto const& r = *walk.range;
        auto first = flatten_expr(r.first);
        auto last = flatten_expr(r.last);
        if (!is_valid(first) || !is_valid(last))
        {
            mark_failed();
            return;
        }
        auto const where = loop.iterable;
        auto const type = entry.at(first).type;
        type_id const ints[] = {type, type};
        auto const literal = [&](i32 v) { return add_expr(type, where, flat_int_literal{.value = v}); };
        // a part that is no constant is held in a local, so the bounds are evaluated once and in order
        auto const held = [&](flat_expr_id value, cc::string_view name)
        {
            if (constant_int(value).has_value())
                return value;
            auto const local = add_local(local_kind::temporary, name, type);
            add_stmt(from, flat_let{.local = local, .value = value});
            return local_ref(local, where);
        };
        auto const op = [&](cc::string_view spelling, flat_expr_id a, flat_expr_id b)
        {
            auto const x = constant_int(a);
            auto const y = constant_int(b);
            if (x.has_value() && y.has_value())
            {
                auto const l = i64(x.value());
                auto const m = i64(y.value());
                auto const v = spelling == "+" ? l + m : spelling == "-" ? l - m : spelling == "*" ? l * m : l / m;
                return literal(i32(v));
            }
            flat_expr_id const operands[] = {a, b};
            return builtin_call(where, c.find_operator(file(), spelling, ints), operands);
        };
        first = held(first, "first");
        last = held(last, "last");
        auto const is_inclusive = sgl::is_valid(r.op) && c.text_of(file(), c.file_of(file()).at(r.op).where) == "..=";
        auto count = held(
            op("-", is_inclusive ? op("+", again(last, where), literal(1)) : again(last, where), again(first, where)),
            "count");
        auto start = first;
        auto step = 1;
        for (auto const s : walk.steps)
        {
            if (!ast::is_valid(s))
            {
                // the last value first: `start + (count - 1) * step`
                start = held(
                    op("+", again(start, where), op("*", op("-", again(count, where), literal(1)), literal(step))),
                    "start");
                step = -step;
                continue;
            }
            auto const by = constant_int(flatten_expr(s));
            if (!by.has_value() || by.value() < 1)
            {
                mark_failed();
                return;
            }
            // as many values as a step of `by` reaches, rounded up; a count of none or fewer stays one of none
            count = held(op("/", op("+", again(count, where), literal(by.value() - 1)), literal(by.value())), "count");
            step *= by.value();
        }
        pending_walk = walk_mapping{.start = start, .step = step};
        if (is_unrolled(id))
            return flatten_unrolled_for(from, id, loop, literal(0), count);
        flatten_looped_for(from, id, loop, literal(0), count);
    }

    void flatten_for(origin from, ast::stmt_id id, ast::for_stmt const& loop)
    {
        auto const walk = ast::is_valid(loop.iterable) ? c.range_walk_of(file(), loop.iterable) : checker::range_walk();
        auto const* const r = walk.range;
        if (r == nullptr)
        {
            mark_failed();
            return;
        }
        if (!walk.steps.empty())
            return flatten_walked_for(from, id, loop, walk);
        auto const first = flatten_expr(r->first);
        auto const end = flatten_expr(r->last);
        if (sgl::is_valid(r->op) && c.text_of(file(), c.file_of(file()).at(r->op).where) == "..=")
            return flatten_inclusive_for(from, id, loop, first, end);
        if (is_unrolled(id))
            return flatten_unrolled_for(from, id, loop, first, end);
        flatten_looped_for(from, id, loop, first, end);
    }

    void flatten_stmt(ast::stmt_id id)
    {
        auto const& s = ast().at(id);
        auto const from = origin{.file = file(), .stmt = id};
        auto const at = cursor_scope(*this, from);
        if (auto const* const let = s.node.try_as<ast::let_stmt>())
        {
            auto const* const name
                = ast::is_valid(let->pattern) ? ast().at(let->pattern).node.try_as<ast::name>() : nullptr;
            if (name == nullptr)
            {
                mark_failed();
                return;
            }
            // minted ahead of its value, so the name the program wrote is its own and a callee inlined there mints past it
            auto const local = add_local(let->is_mut ? local_kind::var : local_kind::let,
                                         c.text_of(file(), name->where), tables().type_at(let->pattern));
            // CHK-55: a `let mut` without a value holds nothing until it is assigned, which judge_assignments holds to
            auto const value = ast::is_valid(let->value) || !let->is_mut ? flatten_expr(let->value) : flat_expr_id::none;
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
        // A test in a function body never runs where it stands (CHK-224), and a `require` and a `const` are no code.
        if (auto const* const d = s.node.try_as<ast::decl_stmt>();
            d != nullptr && ast::is_valid(d->declaration)
            && (ast().at(d->declaration).node.is<ast::test_decl>() || ast().at(d->declaration).node.is<ast::require_decl>()
                || ast().at(d->declaration).node.is<ast::const_decl>()))
            return;
        if (auto const* const a = s.node.try_as<ast::assert_stmt>())
            return flatten_check(from, a->condition, true);

        auto const* const e = s.node.try_as<ast::expr_stmt>();
        if (e == nullptr || !ast::is_valid(e->value))
        {
            mark_failed();
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
                mark_failed();
                return;
            }
            if (!ast::is_valid(b->value))
                return add_stmt(from, flat_leave{.target = loops.back().loop});
            // by value: flattening the value may open a loop of its own
            auto const target = loops.back().value_block;
            is_failed = is_failed || !is_valid(target);
            return add_stmt(from, flat_leave{.target = target, .value = flatten_expr(b->value)});
        }
        if (x.node.is<ast::discard_expr>())
        {
            // CHK-277: only a pixel entry point may reach it, which is known once the whole body is inlined
            discards.push_back({.file = file(), .expr = value});
            return add_stmt(from, flat_discard{});
        }
        if (x.node.is<ast::continue_expr>())
        {
            if (loops.empty())
            {
                mark_failed();
                return;
            }
            if (is_valid(loops.back().continue_block))
                return add_stmt(from, flat_leave{.target = loops.back().continue_block});
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
                mark_failed();
                return;
            }
            // by value: flattening the value may open a value block of its own
            auto const target = blocks.back();
            auto const value = flatten_expr(y->value);
            return add_stmt(from, flat_leave{.target = target, .value = value});
        }
        if (auto const* const c = x.node.try_as<ast::case_expr>())
            return add_stmt(from, flatten_case_parts(*c, label_id::none));
        if (auto const* const i = x.node.try_as<ast::if_expr>())
            return flatten_if_branches(from, ast().at(i->branches), label_id::none);

        // CHK-225: a line of type bool of the test's own body is a check.
        if (is_test && frames.size() == 1 && tables().type_at(value) == bool_type())
            return flatten_check(from, value, false);

        // What is left is a call, and one that returns `void` is a block that is a statement.
        // A line that is no call computes nothing, which the check pass reported; it is evaluated and dropped.
        auto const* const call = x.node.try_as<ast::call>();
        if (call == nullptr)
            return add_stmt(from, flat_eval{.value = flatten_expr(value)});
        auto const& where = tables().target_at(value);
        auto const record = tables().call_at(value);
        if (where.kind == target_kind::overload && record >= 0 && is_inlined(where.symbol)
            && tables().type_at(value) == checked_module::void_type)
        {
            auto const& r = c.out.call_records[record];
            auto const values = flatten_written(c.out.at(r.written));
            auto const inlined = inline_bound(value, where.symbol, values, c.out.at(r.slots));
            return add_stmt(from, flat_block{.label = inlined.label, .body = inlined.body});
        }
        // Any other call has a value, which is evaluated where it stands and dropped.
        add_stmt(from, flat_eval{.value = flatten_expr(value)});
    }

    /// Far beyond any program; it bounds the work on a tree the check pass should never have let through.
    static constexpr isize k_max_inline_depth = 256;
};

/// CHK-389: once, however many trees inline the function the loop stands in.
/// A loop of an inlined function names the call it was reached through, whose arguments may be what its bounds read.
located_diagnostic& report_unroll(checker& c, flattener::unroll_report const& u)
{
    for (auto& d : c.out.diagnostics)
        if (d.what.kind == u.kind && d.file == u.file && d.what.where == u.where)
            return d;
    auto& d = c.report(u.kind, u.file, u.where, u.detail);
    if (ast::is_valid(u.site.call))
        d.notes.push_back({.file = u.site.file,
                           .where = c.span_of(u.site.file, u.site.call),
                           .message = "reached through this call, whose arguments stand in for its parameters"});
    return d;
}
} // namespace

cc::set<cc::string> const* checker::module_names()
{
    if (out.module_names.empty() || module_names_for != out.symbols.size())
    {
        auto names = cc::make_shared<cc::set<cc::string>>();
        for (auto const& s : out.symbols)
            if (!s.name.empty())
                (void)names->insert(s.name);
        out.module_names.push_back(cc::move(names));
        module_names_for = out.symbols.size();
    }
    return out.module_names.back().get();
}

void checker::flatten_test(i32 index)
{
    // by value: flattening appends to the module's vectors
    auto const test = out.tests[index];
    auto const info = out.at(test.symbol).info;
    // One whose text an earlier phase found an error in has been reported already: flattening it could only add a
    // second diagnostic to the first.
    if (has_syntax_error_in(test.file, test.extent))
        return;
    if (!notes[info].is_body_sound || !inlines_whole(test.symbol))
        return;

    auto f = flattener{.c = *this, .is_test = true};
    f.entry.name = "test";
    f.entry.function = test.symbol;
    f.entry.result = checked_module::void_type;
    // CHK-333: what the test lists, which its driver binds
    for (auto const binding : out.at(out.functions[out.at(test.symbol).info].bindings))
        f.entry.bindings.push_back(binding);
    f.entry.names.also_taken = module_names();
    f.frames.push_back({.function = test.symbol, .file = test.file, .result = checked_module::void_type});

    auto const& body = ast_of(test.file).at(test.declaration).node.as<ast::test_decl>().body;
    for (auto const stmt : ast_of(test.file).at(body.statements))
        f.flatten_stmt(stmt);
    // CHK-265: the test's `require` and a helper's that the test reaches are used by the calls that need them
    for (auto const& u : f.feature_uses)
        mark_requires_used(u.within, u.features);

    // Reported for a test that expects it too, which is how a corpus file pins the rule.
    // There it stands at the test's own call, so a helper the test reaches it through carries no error of the test's.
    auto const report_reached = [&](stage_violation const& v, cc::string_view why)
    {
        auto const site = test.expects_diagnostics() ? v.written : call_site{.file = v.file, .call = v.call};
        unsupported(site.file, span_of(site.file, site.call),
                    cc::format("a test that reaches {}{}", out.at(v.callee).name, why));
    };
    // a pipeline's trace or callable runs against tables a test has none of
    for (auto const& v : f.ray_stage_calls)
        report_reached(v, ", which only the ray-tracing stages run");
    // CHK-379
    for (auto const& v : f.subgroup_calls)
        report_reached(v, ": a run is one invocation, and has no subgroup");

    // what a test reaches is of every compile, even where an entry point's tree judged it first
    for (auto const& u : f.unroll_reports)
        report_unroll(*this, u).entry_points.clear();

    // A test that expects a diagnostic is never run (CHK-232), so its tree is judged for its constants alone.
    // Nothing else of the tree is reported: what it expects stands in its text, where the check pass found it already.
    if (test.expects_diagnostics())
    {
        if (!f.is_failed)
        {
            f.entry.body = f.root_body();
            judge_constants(f.entry);
            judge_assignments(f.entry);
        }
        return;
    }

    // CHK-227: once, however many trees inline the function the assert stands in
    for (auto const& a : f.effectful_asserts)
        report_once(diagnostic_kind::unsupported_yet, a.file, span_of(a.file, a.expr),
                    "an assert whose condition writes a buffer, prints, or calls a builtin with an effect")
            .entry_points.clear();
    for (auto const& v : f.stage_violations)
        report(diagnostic_kind::stage_not_allowed, v.file, span_of(v.file, v.call),
               cc::format("{} takes derivatives across a quad of pixels, and a test runs one invocation",
                          out.at(v.callee).name));
    if (f.is_failed && !f.meets_error)
        unsupported(test.file, test.where, "a test whose body reaches a construct the flat tree cannot hold yet");
    if (f.is_failed || !f.stage_violations.empty() || !f.ray_stage_calls.empty() || !f.subgroup_calls.empty())
        return;
    f.entry.body = f.root_body();
    judge_constants(f.entry);
    judge_assignments(f.entry);
    out.tests[index].unit = i32(out.test_units.size());
    out.test_units.push_back(cc::move(f.entry));
}

checker::const_evaluation checker::evaluate_const(symbol_id scope, i32 file, ast::expr_id expr, type_id type)
{
    // Bodies are checked once every signature is known, and inlining needs them, so a const checks those it reaches
    // first; recursion among them is find_recursion's to report, and stops this walk at the first repeat.
    auto reached = cc::vector<symbol_id>{scope};
    for (auto i = isize(0); i < reached.size(); ++i)
        for (auto k = isize(0); k < calls.size(); ++k)
        {
            if (calls[k].caller != reached[i])
                continue;
            auto const callee = calls[k].callee;
            auto is_known = false;
            for (auto const r : reached)
                is_known = is_known || r == callee;
            if (is_known)
                continue;
            reached.push_back(callee);
            if (out.at(callee).state == symbol_state::checked)
                check_body(callee);
        }

    auto f = flattener{.c = *this};
    f.entry.name = "const";
    f.entry.function = scope;
    f.entry.result = type;
    f.entry.names.also_taken = module_names();
    f.frames.push_back({.function = scope, .file = file, .result = type});
    auto const result = f.flatten_expr(expr);
    if (f.is_failed || !is_valid(result))
        return {.detail = "a const whose value reaches a construct the flat tree cannot hold yet"};
    f.add_stmt(origin{.file = file, .expr = expr}, flat_return{.value = result});
    f.entry.body = f.root_body();

    // a const reads no binding, so the run has no inputs, and a value that takes this long is no constant
    constexpr auto fuel = 100'000;
    auto const o = interpret(out, f.entry, {}, {.fuel = fuel});
    auto const without_value = [](cc::string detail)
    { return const_evaluation{.kind = diagnostic_kind::constant_without_value, .detail = cc::move(detail)}; };
    switch (o.status)
    {
    case run_status::ok:
        return {.leaves = o.result.leaves, .is_ok = true};
    case run_status::out_of_fuel:
        return without_value(cc::format("takes more than {} steps to compute", fuel));
    case run_status::assertion_failed:
        return without_value("fails an assert while it is computed");
    case run_status::program_error:
        return without_value(cc::format("has no value: {}", o.detail));
    default:
        return {.detail = cc::format("a const whose value the abstract machine cannot compute: {} {}",
                                     to_string(o.status), o.detail)};
    }
}

bool checker::is_sound(type_id type) const
{
    if (!is_valid(type) || type == checked_module::error_type)
        return false;
    for (auto const& m : out.at(out.at(type).members))
        if (!is_sound(m.type))
            return false;
    return true;
}

void checker::flatten_entry_point(symbol_id id, traversal_request const* traversal)
{
    auto const& s = out.at(id);
    auto const& info = out.functions[s.info];
    auto const& note = notes[s.info];
    if (info.entry_stage == stage::none || !note.is_valid_entry || !inlines_whole(id))
        return;

    auto const parameters = out.at(info.parameters);
    // A compute entry point hands nothing back, so `nothing` is its result and not a hole.
    auto const wants_result = info.entry_stage != stage::compute;
    auto is_parameter_sound = true;
    for (auto const& parameter : parameters)
        is_parameter_sound = is_parameter_sound && is_sound(parameter.type);
    if (!is_parameter_sound || (wants_result && !is_sound(info.result)))
        return;

    auto f = flattener{.c = *this};
    f.entry.entry_stage = info.entry_stage;
    f.entry.name = traversal != nullptr ? traversal->name : cc::string(s.name);
    f.entry.function = id;
    // CHK-271: the stage struct, when there is one, is the first parameter; every other is a stage input
    if (!parameters.empty() && parameters[0].input == stage_input::none)
        f.entry.input = parameters[0].type;
    f.entry.result = info.result;
    f.entry.workgroup[0] = info.workgroup[0];
    f.entry.workgroup[1] = info.workgroup[1];
    f.entry.workgroup[2] = info.workgroup[2];
    f.entry.preferred_subgroup_size = info.preferred_subgroup_size;
    f.entry.features = info.features;
    for (auto const binding : out.at(info.bindings))
        f.entry.bindings.push_back(binding);

    // Every module-level name is taken, so no local can hide a type, a binding or a builtin an emitter writes.
    f.entry.names.also_taken = module_names();
    f.entry.names.reserve(s.name);

    f.frames.push_back({.function = id, .file = s.file, .result = info.result});
    // CHK-326: a ray-tracing stage's one parameter of the target is its payload, `locals[0]`, which the stage writes
    // through; its launch, its ray and its hit it reads from the target where the entry point starts
    if (info.entry_stage >= stage::raygen)
    {
        f.entry.input = type_id::none;
        // CHK-345: a traversal function takes the payload its any hit writes, which no intersection is handed
        if (traversal != nullptr)
        {
            f.fused = flattener::fusion{.any_hit = traversal->any_hit};
            if (is_valid(traversal->payload))
            {
                f.fused.value().payload = f.add_local(local_kind::parameter, "payload", traversal->payload);
                f.entry.input = traversal->payload;
            }
        }
        for (auto const& parameter : parameters)
            if (parameter.is_mut)
            {
                auto const local = f.add_local(local_kind::parameter, parameter.name, parameter.type);
                f.entry.input = parameter.type;
                f.current()->bound.push_back(
                    {.where = {.kind = target_kind::parameter, .index = i32(parameter.field)}, .local = local});
            }
        for (auto const& parameter : parameters)
        {
            if (parameter.is_mut)
                continue;
            // CHK-342: a procedural hit or candidate carries the attributes the target hands the stage
            auto const& t = out.at(parameter.type);
            if (is_valid(t.generic))
            {
                auto const is_hit = out.at(t.symbol).name == "procedural_hit";
                auto const* const found
                    = prelude_names.get_ptr(is_hit ? cc::string_view("current_procedural_hit")
                                                   : cc::string_view("current_procedural_candidate"));
                if (found == nullptr || found->empty())
                {
                    f.mark_failed();
                    continue;
                }
                auto const builder = found->front();
                auto const attributes = f.add_local(local_kind::parameter, "attributes", t.element);
                f.entry.attributes = attributes;
                type_id const bindings[] = {out.at(out.functions[out.at(builder).info].type_parameters)[0], t.element};
                flat_expr_id const arguments[] = {f.local_ref(attributes, ast::expr_id::none)};
                i32 const slots[] = {0};
                auto const inlined = f.inline_bound(ast::expr_id::none, builder, arguments, slots, {}, bindings);
                auto const value = f.add_expr(parameter.type, ast::expr_id::none,
                                              flat_block{.label = inlined.label, .body = inlined.body});
                auto const local = f.add_local(local_kind::let, parameter.name, parameter.type);
                f.add_stmt({.file = s.file, .expr = ast::expr_id::none}, flat_let{.local = local, .value = value});
                f.current()->bound.push_back(
                    {.where = {.kind = target_kind::parameter, .index = i32(parameter.field)}, .local = local});
                continue;
            }
            auto const source
                = parameter.input == stage_input::launch_id           ? cc::string_view("launch_index")
                : parameter.input == stage_input::launch_size         ? cc::string_view("launch_dimensions")
                : out.name_of(parameter.type) == "ray"                ? cc::string_view("current_ray")
                : out.name_of(parameter.type) == "triangle_hit"       ? cc::string_view("current_triangle_hit")
                : out.name_of(parameter.type) == "triangle_candidate" ? cc::string_view("current_triangle_candidate")
                : out.name_of(parameter.type) == "procedural_box"     ? cc::string_view("current_procedural_box")
                                                                      : cc::string_view();
            auto const* const found = prelude_names.get_ptr(source);
            if (source.empty() || found == nullptr || found->empty())
            {
                f.mark_failed();
                continue;
            }
            auto const read = found->front();
            auto value = flat_expr_id::none;
            if (is_valid(out.at(read).intrinsic))
                value = f.builtin_call(ast::expr_id::none, read, {});
            else
            {
                auto const inlined = f.inline_call(ast::expr_id::none, read, {});
                value = f.add_expr(parameter.type, ast::expr_id::none,
                                   flat_block{.label = inlined.label, .body = inlined.body});
            }
            auto const local = f.add_local(local_kind::let, parameter.name, parameter.type);
            f.add_stmt({.file = s.file, .expr = ast::expr_id::none}, flat_let{.local = local, .value = value});
            f.current()->bound.push_back(
                {.where = {.kind = target_kind::parameter, .index = i32(parameter.field)}, .local = local});
            if (f.fused.has_value() && source == "current_procedural_box")
                f.fused.value().box = local;
        }
    }
    // Every parameter is a local, in the order written: the stage struct at `locals[0]` when there is one.
    for (auto const& parameter : info.entry_stage >= stage::raygen ? cc::span<check::parameter const>() : parameters)
    {
        auto const local = f.add_local(local_kind::parameter, parameter.name, parameter.type);
        f.current()->bound.push_back(
            {.where = {.kind = target_kind::parameter, .index = i32(parameter.field)}, .local = local});
        if (parameter.input != stage_input::none)
            f.entry.stage_inputs.push_back({.input = parameter.input, .local = local});
    }

    auto const& body = ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().body;
    if (ast::is_valid(body.value))
        f.flatten_return({.file = s.file, .expr = body.value}, body.value);
    for (auto const stmt : ast_of(s.file).at(body.statements))
        f.flatten_stmt(stmt);

    // CHK-322: what the body calls needs a device's feature, which the entry point declares like one of its signature
    // CHK-402: what is judged of this entry point's inlined body is its own, which a compile of a sibling leaves out.
    // One that another tree judged first without an entry point, a test's, stays of every compile.
    auto const of_entry = [&](auto&& make) -> located_diagnostic&
    {
        auto const before = out.diagnostics.size();
        auto& d = make();
        if ((out.diagnostics.size() > before || !d.entry_points.empty()) && !d.entry_points.values().contains(id))
            d.entry_points.push_back(id);
        return d;
    };
    // once, however many entry points inline the function it stands in, the same detail naming every one of them
    auto const report_of_entry
        = [&](diagnostic_kind kind, i32 file, source_span where, cc::string detail) -> located_diagnostic&
    {
        return of_entry(
            [&]() -> located_diagnostic&
            {
                for (auto& d : out.diagnostics)
                    if (d.what.kind == kind && d.file == file && d.what.where == where && d.detail == detail)
                        return d;
                return report(kind, file, where, cc::move(detail));
            });
    };

    auto used = feature_set();
    for (auto const& u : f.feature_uses)
        used |= u.features;
    auto const declared = notes[s.info].declared_features;
    for (auto i = isize(0); i < k_feature_count; ++i)
    {
        auto const needed = feature(i);
        if (!used.has(needed))
            continue;
        if (!declared.has(needed))
        {
            auto& d = report_of_entry(diagnostic_kind::feature_not_declared, s.file,
                                      ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().name,
                                      cc::format("{} needs {}, which neither its file, a binding it lists nor its body "
                                                 "requires",
                                                 s.name, name_of(needed)));
            for (auto const& u : f.feature_uses)
                if (u.features.has(needed))
                {
                    d.notes.push_back({.file = u.file,
                                       .where = span_of(u.file, u.call),
                                       .message = u.is_value ? "a value that needs it" : "the call that needs it"});
                    break;
                }
        }
    }
    // a body's `require` that declares it is used, as one declaring a need of the signature is (CHK-265)
    for (auto const& u : f.feature_uses)
        mark_requires_used(u.within, u.features);
    out.functions[s.info].features |= used;
    f.entry.features |= used;
    for (auto const& u : f.unroll_reports)
        of_entry([&]() -> located_diagnostic& { return report_unroll(*this, u); });
    // CHK-227: once, however many trees inline the function the assert stands in
    for (auto const& a : f.effectful_asserts)
        of_entry(
            [&]() -> located_diagnostic&
            {
                return report_once(diagnostic_kind::unsupported_yet, a.file, span_of(a.file, a.expr),
                                   "an assert whose condition writes a buffer, prints, or calls a builtin with an "
                                   "effect");
            });
    for (auto const& v : f.stage_violations)
    {
        auto const where = span_of(v.file, v.call);
        if (v.decision == stage::none)
        {
            report_of_entry(diagnostic_kind::stage_not_allowed, v.file, where,
                            cc::format("a {} entry point reaches this, and {} is @stages without it",
                                       check::stage_name(info.entry_stage), out.at(v.callee).name));
            continue;
        }
        // CHK-345: a traversal fuses an intersection and an any hit whose own trees reported it already
        if (traversal != nullptr)
            continue;
        // CHK-404: a write runs as often as its function, which traversal decides for a decision
        auto const* const record = v.is_buffer_store ? nullptr : out.builtin_function(out.at(v.callee).intrinsic);
        auto const is_any_hit = v.decision == stage::any_hit;
        auto const what
            = v.is_inlined_decision
                ? cc::format("an inline trace calls this as its {} decision", is_any_hit ? "any-hit" : "intersection")
                : cc::format("this is reached from an {} entry point", check::stage_name(v.decision));
        auto const how_often
            = is_any_hit
                ? cc::string("which runs an implementation-defined number of times: for candidates the traversal "
                             "later rejects, and more than once per candidate unless the geometry sets "
                             "NO_DUPLICATE_ANYHIT_INVOCATION, which Metal has no equivalent of")
                : cc::string("which runs as often as the traversal reaches its box, an implementation-defined "
                             "number of times");
        auto written = cc::string();
        if (v.is_buffer_store)
            written = "this store to a buffer would write a number of times no spec defines, and race with itself";
        else if (record != nullptr && record->is_atomic)
        {
            // an image's texel atomic is the prelude's internal `texel_max`, which the program wrote as `.max`
            auto name = cc::string_view(out.at(v.callee).name);
            if (name.starts_with("texel_"))
                name = name.subview(6);
            written = cc::format("the atomic {} would run a number of times no spec defines", name);
        }
        else
            written = cc::format("the {} would write a number of times no spec defines", out.at(v.callee).name);
        report_of_entry(
            diagnostic_kind::stage_not_allowed, v.file, where,
            cc::format("{}, {}; a decision writes no memory through a binding, so {}", what, how_often, written));
    }
    // CHK-380: HLSL forms a compute stage's quads from its threads' ids, which only one axis keeps consecutive
    auto const is_linear = info.workgroup[0] % 4 == 0 && info.workgroup[1] == 1 && info.workgroup[2] == 1;
    for (auto const& q : is_linear ? cc::span<stage_violation const>() : cc::span<stage_violation const>(f.quad_calls))
        report_of_entry(diagnostic_kind::invalid_entry_point, q.file, span_of(q.file, q.call),
                        cc::format("{} forms quads along one axis, so a compute entry point that reaches it has a "
                                   "workgroup of (x, 1, 1) whose x is a multiple of 4, and one reaching it has ({}, "
                                   "{}, "
                                   "{})",
                                   out.at(q.callee).name, info.workgroup[0], info.workgroup[1], info.workgroup[2]));
    auto const reaches_discard = info.entry_stage != stage::pixel && !f.discards.empty();
    for (auto const& d : info.entry_stage != stage::pixel ? cc::span<origin const>(f.discards) : cc::span<origin const>())
        report_of_entry(diagnostic_kind::stage_not_allowed, d.file, span_of(d.file, d.expr),
                        cc::format("a {} entry point reaches this, and only a pixel stage discards",
                                   check::stage_name(info.entry_stage)));
    if ((info.stages & stage_bit(info.entry_stage)) == 0)
        report_of_entry(diagnostic_kind::stage_not_allowed, s.file,
                        ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().name,
                        cc::format("{} is a {} entry point, and its own @stages leaves that out", s.name,
                                   check::stage_name(info.entry_stage)));
    // CHK-213: a gap of this pass is reported, so an entry point never vanishes without a word.
    if (f.is_failed && !f.meets_error)
    {
        auto const name = ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().name;
        if (!f.first_failure.has_value())
            unsupported(s.file, name,
                        cc::format("{}: its body reaches a construct the flat tree cannot hold yet", s.name));
        else
        {
            // the construct itself, and the entry point it was reached from, however many calls deep
            auto const at = f.first_failure.value();
            auto const where = ast::is_valid(at.expr) ? span_of(at.file, at.expr) : span_of(at.file, at.stmt);
            auto& d = report(diagnostic_kind::unsupported_yet, at.file, where,
                             "a construct the flat tree cannot hold yet, here");
            d.notes.push_back({.file = s.file,
                               .where = name,
                               .message = cc::format("reached from the {} entry point {}",
                                                     check::stage_name(info.entry_stage), s.name)});
        }
    }
    if (f.is_failed || !f.stage_violations.empty() || reaches_discard
        || (info.stages & stage_bit(info.entry_stage)) == 0 || (!is_linear && !f.quad_calls.empty()))
        return;
    f.entry.body = f.root_body();

    // Every later walk stops descending at the limit and leaves a hole where it stopped, so a tree past it must never
    // reach them: the hole would surface as a missing id rather than as the limit (CHK-268).
    auto probe = depth_probe{.e = f.entry};
    probe.body(f.entry.body, 0);
    if (probe.found.has_value())
    {
        auto const& at = probe.found.value();
        auto const where = ast::is_valid(at.expr) ? span_of(at.file, at.expr) : span_of(at.file, at.stmt);
        report(diagnostic_kind::nesting_too_deep, at.file, where,
               cc::format("'{}' nests deeper than {} levels here, with every call inlined", s.name, k_max_depth));
        return;
    }
    judge_constants(f.entry);
    judge_assignments(f.entry);
    judge_uniformity(f.entry);
    f.entry.options = options_reached(id, traversal != nullptr ? traversal->any_hit : symbol_id::none);
    out.entry_points.push_back(cc::move(f.entry));
}

void checker::flatten_metal_traversals()
{
    auto is_any = false;
    for (auto const& p : out.pipelines)
    {
        if (p.kind != pipeline_kind::hit_group && p.kind != pipeline_kind::raytracing)
            continue;
        is_any = true;
        if (p.kind != pipeline_kind::hit_group || !p.is_procedural || out.at(p.symbol).file != i32(out.files.size()) - 1)
            continue;
        auto const rays = out.at(out.at(out.at(p.ray_set).type).members);
        auto const records = out.at(p.records);
        for (auto r = isize(0); r < rays.size(); ++r)
        {
            auto const request = traversal_request{.name = cc::format("sgl_{}_{}", out.at(p.symbol).name, rays[r].name),
                                                   .any_hit = records[r * 2 + 1],
                                                   .payload = rays[r].type};
            flatten_entry_point(p.intersection, &request);
        }
    }
    // CHK-345: a record without a closest hit calls this one on metal, where a table slot holds a function or crashes
    // it stands for no function of the source, so it is owned by a ray-tracing function of the program
    auto owner = symbol_id::none;
    for (auto const& fn : out.functions)
        if (fn.entry_stage >= stage::raygen && out.at(fn.symbol).file == i32(out.files.size()) - 1)
            owner = fn.symbol;
    if (!is_any || !is_valid(owner))
        return;
    auto f = flattener{.c = *this};
    f.entry.entry_stage = stage::closest_hit;
    f.entry.function = owner;
    f.entry.name = "sgl_empty_closest_hit";
    f.entry.result = checked_module::void_type;
    f.entry.features = feature_set(feature::raytracing_pipeline);
    f.entry.names.reserve(f.entry.name);
    f.frames.push_back({.file = i32(out.files.size()) - 1});
    f.entry.body = f.root_body();
    out.entry_points.push_back(cc::move(f.entry));
}
