#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/set.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/interpret/interpret.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

// The flattener of check/flatten.cc, which flatten_expr.cc, flatten_call.cc and flatten_stmt.cc define the rest of.

namespace sgl::check::impl
{
/// True where evaluating `id` writes what outlives it: a builtin call that is not pure.
bool writes_outside(flat_entry_point const& e, flat_expr_id id, int depth);
/// True where running `body` writes what outlives it: a print, a store to a buffer element, or an impure builtin.
bool writes_outside(flat_entry_point const& e, ast::range_of<flat_stmt_id> body, int depth);

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
    /// The callee is a builtin only a test calls (CHK-512).
    bool is_test_only = false;
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
    /// A compute entry point's calls of a quad operation, whose workgroup then forms its quads along one axis (CHK-380).
    cc::vector<stage_violation> quad_calls;
    /// A compute entry point's calls of a barrier or a subgroup operation, which read its group: a bare `@compute`
    /// reaches none (CHK-520), and one that reaches one has a size no warning judges (CHK-521).
    cc::vector<stage_violation> group_calls;
    /// The condition of every `assert` whose run would write what outlives it, which its caller reports (CHK-227).
    cc::vector<origin> effectful_asserts;
    /// Every `discard` the tree reaches, which only a pixel entry point may (CHK-277).
    cc::vector<origin> discards;
    /// An unrolled `for` the tree did not unroll, which its caller reports once (CHK-389).
    /// One whose bound is no constant is an error, and one that would write too many copies is a loop instead, and a warning.
    /// A `const if` whose condition is no constant is reported here too, as an error (CHK-425).
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
    /// The features the `has` conditions of the branches being flattened grant (CHK-426).
    feature_set granted;
    /// The tree is a const's value, which reads no compile's target (CHK-426).
    bool is_const_evaluation = false;
    /// Why a const's value read what a compile stands for; empty where it read nothing of it.
    cc::string target_in_const;
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
    /// A pixel entry point's minted `@pixel struct`, which each of its own returns builds of the one color it gives
    /// (CHK-418); none for every other tree.
    type_id pixel_targets = type_id::none;
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
        /// What the `has` of the branches around the use granted, which then needs no `require` (CHK-426).
        feature_set granted;
    };
    cc::vector<feature_use> feature_uses;

    /// CHK-382: a value of `type` at `from` needs what the type needs, once per function it stands in.
    void note_value_use(type_id type, ast::expr_id from);
    /// CHK-345: an intersection entry point fused with the any hit of one record, which its own `return` runs.
    struct fusion
    {
        symbol_id any_hit = symbol_id::none;
        local_id payload = local_id::none;
        local_id box = local_id::none;
        local_id ends_search = local_id::none;
    };
    cc::optional<fusion> fused;

    /// Notes a call of `callee` whose `@stages` leaves out the stage of the entry point being flattened.
    /// A test has no stage, so it may reach what any stage may, but a builtin only the ray-tracing stages run.
    void judge_stage(ast::expr_id call, symbol_id callee);
    /// What a traversal decides how often to run, which then writes no memory through a binding (CHK-404): an
    /// `@any_hit` or `@intersection` entry point, or the decision an inline trace's loop is calling.
    [[nodiscard]] stage decision_stage() const;

    /// CHK-404: an assignment to a buffer's element, or to a part of one, from a decision.
    void judge_decision_store(flat_expr_id place, ast::expr_id target);

    flat_entry_point entry;
    /// The tree is a test, whose own lines of type bool are checks (CHK-225); a function it calls has none.
    bool is_test = false;
    /// The index of every `for` of the test's own body around the statement being written, outermost first.
    cc::vector<local_id> test_loops;
    bool is_failed = false;
    /// The tree met the error type somewhere, so a failure has a diagnostic already (CHK-7).
    bool meets_error = false;

    /// True where the error type stands in `type` at any depth.
    [[nodiscard]] bool holds_error(type_id type) const;

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
        /// The instance whose body it was written in, whose tables hold its types; -1 outside one (CHK-338).
        i32 instance = -1;
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
    /// CHK-435: the entry point keeps the strictest `@float` of every function whose body it inlines.
    void raise_float_level(symbol_id function)
    {
        if (!is_valid(function) || index_of(function) >= c.out.symbols.size())
            return;
        auto const& s = c.out.at(function);
        if (s.info < 0 || s.info >= c.out.functions.size())
            return;
        auto const level = c.out.functions[s.info].float_guarantee;
        if (u8(level) > u8(entry.float_guarantee))
        {
            entry.float_guarantee = level;
            entry.float_raised_by = function;
        }
    }

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
        /// The instance of the generic function being written, whose tables its body reads; -1 for none (CHK-338).
        i32 instance = -1;
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

    [[nodiscard]] frame_ref current();

    /// The statements of the list being written.
    cc::vector<flat_stmt_id> block;

    /// Read inside ONE expression, which no push can come between; everything else goes through `current`.
    [[nodiscard]] i32 file() const { return frames.back().file; }
    [[nodiscard]] ast::file_ast const& ast() const { return c.ast_of(file()); }
    [[nodiscard]] table_view tables() const
    {
        return {.module = &c.out, .file = file(), .instance = frames.back().instance};
    }

    flat_expr_id fail();

    /// The expression or statement being flattened, innermost, which a failure is located at (CHK-213).
    origin cursor;
    /// Where the first failure stood: what a report names, however deep the construct is.
    cc::optional<origin> first_failure;

    void mark_failed();

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
    [[nodiscard]] type_id concrete(type_id type);

    local_id add_local(local_kind kind, cc::string_view desired, type_id type);

    /// The names of `entry.labels`, which are unique among the labels alone.
    name_mint label_names;

    label_id add_label(cc::string_view desired);

    template <class Node>
    flat_expr_id add_expr(type_id type, ast::expr_id from, Node node)
    {
        // A field or a swizzle of a swizzle reads the operand below it, so a place stays one assignable target.
        // That is what a `mut` parameter bound to a swizzle meets, wherever its body names a field of it (CHK-352).
        if constexpr (std::is_same_v<Node, flat_member>)
            if (is_valid(node.object))
                if (auto const* const inner = entry.at(node.object).node.template try_as<flat_member>();
                    inner != nullptr && inner->is_swizzle())
                {
                    auto const outer
                        = node.is_swizzle() ? node.letters : swizzle{.fields = {i8(node.member)}, .count = 1};
                    auto const composed = outer.over(inner->letters);
                    node = composed.count == 1 ? flat_member{.object = inner->object, .member = composed.fields[0]}
                                               : flat_member{.object = inner->object, .letters = composed};
                }
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

    ast::range_of<flat_expr_id> add_list(cc::span<flat_expr_id const> list);

    ast::range_of<flat_stmt_id> add_list(cc::span<flat_stmt_id const> list);

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
    ast::range_of<flat_stmt_id> root_body();

    ast::range_of<flat_arm> add_arms(cc::span<flat_arm const> list);

    flat_expr_id local_ref(local_id local, ast::expr_id from);

    /// True for what may stand in several places and be evaluated in any of them: a literal, or an immutable local.
    [[nodiscard]] bool is_substitutable(flat_expr_id id) const;
    /// True where evaluating `id` reads nothing a body could write, so it gives one value wherever it is evaluated:
    /// literals, immutable locals and pure operations over them.
    [[nodiscard]] bool is_stable(flat_expr_id id, int depth = 0) const;
    /// EMIT-180: the let of `local` in the block being written, if the block reads it once outside a loop and its value
    /// is stable, folded into that one read.
    void substitute_single_use(local_id local);
    /// Conservatively true where statement `id` of a block expression may read `local`.
    [[nodiscard]] bool reads_in(flat_stmt_id id, local_id local) const;

    /// What `nonuniform i` or `uniform i` marks, `i`; none for anything else.
    [[nodiscard]] flat_expr_id marked_index(flat_expr_id id) const;

    /// An index that may stand in several places: a substitutable one, or a mark of one, the mark kept on it.
    [[nodiscard]] bool is_substitutable_index(flat_expr_id id) const;

    /// A construction of literals alone, such as a texel offset, which a target may take only as it is written.
    [[nodiscard]] bool is_literal_construction(flat_expr_id id) const;

    /// One more node that means what the substitutable `id`, or a construction of literals, means, attributed to `from`.
    flat_expr_id again(flat_expr_id id, ast::expr_id from);

    /// `index` as it may stand in several places: itself where it is substitutable, and otherwise a local of `kind`
    /// bound at `from`.
    /// A marked `i` binds `i` and marks the bound local, since a mark is read only where it indexes (CHK-300).
    flat_expr_id bound_index(flat_expr_id index, local_kind kind, cc::string_view name, origin from);

    /// `again` for an index, which keeps the place it was written: a diagnostic about the index of an element a
    /// parameter stands for belongs to the caller that wrote it, not to wherever the callee names the parameter.
    flat_expr_id again_as_written(flat_expr_id index, ast::expr_id from);

    /// `place` with every index it holds evaluated now, into a local where it is no substitutable value, so the place
    /// can stand wherever a `mut` parameter is named and mean the same element each time (CHK-316).
    flat_expr_id pin_place(flat_expr_id place);

    /// True for a texture, an image, a sampler or a buffer read from its binding, and a file-scope sampler: it is no
    /// value a local could hold, and it stands wherever it is named, since naming one has no effect.
    [[nodiscard]] bool is_resource_member(flat_expr_id id) const;

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
    evaluated_once evaluate_once(flat_expr_id value, cc::string_view name, ast::expr_id from);

    // ---- swizzles ---------------------------------------------------------------------------------------------------

    /// The fields of `operand` a swizzle reads.
    struct swizzled
    {
        ast::expr_id operand = ast::expr_id::none;
        swizzle letters;
    };

    /// True for the member `id` that is a swizzle, or a field of one.
    [[nodiscard]] bool is_swizzled(ast::expr_id id) const;

    /// What the `is_swizzled` member `id` reads, through every swizzle below it: `v.zyx.yx` reads `v.yz`, and `v.zy.x`
    /// reads `v.z`.
    [[nodiscard]] swizzled swizzled_at(ast::expr_id id) const;

    /// The type of field `field` of a value of `type`.
    [[nodiscard]] type_id field_type(type_id type, i32 field) const;

    /// One field of `object`, or the swizzle `letters` of it, which is `type`.
    /// A prelude vector's swizzle is a member of its own; a struct of the program's is the construction of the vector it
    /// means, from an operand that is no local evaluated once (EMIT-142).
    flat_expr_id swizzle_of(flat_expr_id object, swizzle letters, type_id type, ast::expr_id id);

    /// CHK-391: `value as texel[F]` of the texel type `type` and of another width: its leading components, or
    /// all of them and the rest of (0, 0, 0, 1), from an operand that is evaluated once.
    flat_expr_id texel_conversion(flat_expr_id value, type_id type, ast::expr_id id);

    flat_expr_id read_swizzle(swizzled const& s, type_id type, ast::expr_id id);

    /// `v.zy = value`, and `v.zy op= value`, whose operand's indices are evaluated once, before the value (CHK-352).
    void flatten_swizzle_assign(origin from, ast::assign_stmt const& assign, cc::string_view op);

    // ---- expressions ------------------------------------------------------------------------------------------------

    flat_expr_id flatten_expr(ast::expr_id id);

    /// A case of `type`; a case of a builtin enum is a literal of what the targets write it as: a bool, or an int.
    flat_expr_id enum_value(type_id type, ast::expr_id from, i32 case_index);

    /// A const stands for its value, written where the name stood.
    flat_expr_id constant_value(type_id type, ast::expr_id from, constant_info const& info);

    /// The scalars of a computed const from `at` on, built back into a value of `type` in the shape every target
    /// constructs one: a vector of its scalars, a matrix of its columns, a struct of its members and an array of its
    /// elements.
    flat_expr_id value_of(type_id type, ast::expr_id from, cc::span<scalar const> leaves, isize& at);

    /// A number literal as the type it was checked as, which a conversion may have made other than its default.
    flat_expr_id flatten_number(ast::expr_id id, type_id type);

    /// What an operator over number literals folded to, as the type it was checked as.
    flat_expr_id flatten_folded(ast::expr_id id, type_id type, number_literal const& n);
    /// `lhs == rhs` of two arrays, or `!=` where not `is_equal`: a loop over the elements, each side evaluated once.
    flat_expr_id flatten_array_equality(ast::expr_id id, type_id type, bool is_equal, flat_expr_id lhs, flat_expr_id rhs);

    /// `[a, b, c]` of an array type, its elements in the order written (EVAL-91).
    flat_expr_id flatten_array_literal(ast::expr_id id, type_id type, ast::array const& literal);

    /// CHK-289: `T[N].filled(v)` is a local every element of which is assigned `v`, evaluated once.
    /// A nested array filled with its innermost element assigns `v` to every scalar, one loop per level.
    flat_expr_id flatten_filled(ast::expr_id id, type_id type, ast::call const& call);

    flat_expr_id flatten_call(ast::expr_id id, type_id type, target const& where, ast::call const& call);

    /// CHK-329: `trace(world, r, set.ray, mut p)` is the target's trace, with the ray's position in its set as its
    /// contribution and its miss, and the set's size as its multiplier.
    /// The payload is handed over as a local the call writes through, copied in and out where it is another place.
    flat_expr_id flatten_ray_trace(ast::expr_id id, ast::call const& call, ray_trace const& t);
    /// `h.invoke(mut p)`: the hit object's shaders, run on the payload (CHK-434).
    flat_expr_id flatten_hit_invoke(ast::expr_id id, ast::call const& call);

    /// CHK-345: a report beyond the ray's range is none, and one in it is what the any hit decides of it.
    flat_expr_id decide_report(origin from, flat_expr_id reported);

    /// CHK-344: `table[i](mut p)` is a call of callable `base + i` of the pipeline's section, where `base` is where the
    /// module's tables before this one end: they pack in declaration order.
    flat_expr_id flatten_callable_call(ast::expr_id id, ast::call const& call, callable_call const& t);

    /// The element `place` names, over a local holding its index where evaluating the index twice could differ.
    flat_expr_id with_bound_index(flat_expr_id place, ast::expr_id id);

    /// True where a call of a program function is inlined rather than written as a call of the target.
    [[nodiscard]] bool is_inlined(symbol_id callee) const;

    /// A call as its record says: the written arguments evaluated in the order written, each filling its parameter.
    flat_expr_id flatten_bound_call(ast::expr_id id, type_id type, call_record const& record);

    /// The subscript `img[xy]` of an `@atomic` image a call updates as its first argument; none for any other call.
    [[nodiscard]] ast::expr_id atomic_texel_of(call_record const& record) const;

    /// CHK-373: `img[xy].max(v)` is the texel's own atomic, which takes the image, its coordinates and the operands.
    /// The coordinates are those of the plain image's `load`, which the check pass resolved at the subscript.
    flat_expr_id flatten_texel_atomic(ast::expr_id id, type_id type, call_record const& record, ast::expr_id texel);

    /// The prelude's internal atomic `name` of a texel of `image`, by the image's shape and texel.
    [[nodiscard]] symbol_id texel_atomic(cc::string_view name, check::type_info const& image) const;

    /// A builtin or a construction over `values`, written in the order the call wrote them.
    /// Where the parameters take them in that order and no two have an effect, they stand in the call itself.
    /// Otherwise each is bound by a `let` in the order written, since a target may evaluate a call's arguments in any
    /// order (EVAL-82), and the call reads the locals.
    flat_expr_id target_call(ast::expr_id id,
                             type_id type,
                             symbol_id callee,
                             cc::span<flat_expr_id const> values,
                             cc::span<i32 const> slots);

    /// True where evaluating `id` has an effect, an inlined call's body included.
    /// A block whose only leave is its last statement leaves its value where it stood (LEGAL-15).
    /// So an inlined `fun f() => counter.add(1)` is an impure call in the text, written wherever the target puts it.
    [[nodiscard]] bool calls_impure(flat_expr_id id, int depth) const;

    /// `a == b` over void: both sides run for their effects, in order, and the answer is known before either does.
    flat_expr_id flatten_void_equality(ast::expr_id id, type_id type, bool is_equal, ast::expr_id lhs, ast::expr_id rhs);

    flat_expr_id builtin_call(ast::expr_id id, symbol_id callee, cc::span<flat_expr_id const> arguments);

    /// A sampling call without its sampler calls the record that takes one, with the texture's `@sampler` after the
    /// coordinate (CHK-279): a member of its binding, or a file-scope sampler.
    /// The check pass has made sure the texture names one.
    flat_expr_id default_sampled_call(ast::expr_id id, builtin_id with_sampler, cc::span<flat_expr_id const> arguments);

    /// The values of a call's written arguments, in the order written.
    /// A splat stands for one member access per field, and its value is evaluated once: where the first one stands.
    cc::vector<flat_expr_id> flatten_written(cc::span<written_argument const> written);

    /// The closure the function argument `expr` hands over: a lambda, a function's name, or a parameter handed on.
    i32 closure_of(ast::expr_id expr);

    /// `f(args)` where `f` is a parameter of function type: the function it was handed, inlined here (CHK-319).
    flat_expr_id flatten_closure_call(ast::expr_id id, type_id type, i32 handed, call_record const& record);

    /// PROTOTYPE: `fwd_diff` or `bwd_diff` of core.sgl, whose derivative flatten_diff.cc writes in place of its body.
    [[nodiscard]] bool is_diff_call(symbol_id callee) const;
    /// PROTOTYPE: `no_diff` of core.sgl, a value a derivative holds constant.
    [[nodiscard]] bool is_detach_call(symbol_id callee) const;

    flat_expr_id flatten_diff(ast::expr_id id, type_id type, call_record const& record);

    /// Why the tree failed, where a pass knows better than CHK-213's words; empty otherwise.
    cc::string failure_detail;

    /// How many `fwd_diff` and `bwd_diff` are inlining the function they differentiate right now.
    i32 diff_depth = 0;

    /// The derivatives written by hand for a function: `@forward(f)` and `@backward(f)`.
    struct custom_derivatives
    {
        symbol_id forward = symbol_id::none;
        symbol_id backward = symbol_id::none;
    };
    [[nodiscard]] custom_derivatives custom_derivatives_of(symbol_id function) const;

    /// A call, inside a function being differentiated, of a function with a derivative written by hand.
    /// Its arguments are bound to `arguments`, one per parameter, and its block's label keys it in `custom_calls`.
    struct custom_call
    {
        symbol_id function = symbol_id::none;
        custom_derivatives derivatives;
        cc::vector<local_id> arguments;
        ast::expr_id call = ast::expr_id::none;
        /// `no_diff(x)`, whose derivative is zero: its argument is bound inside it, so nothing it reads is followed.
        bool is_detached = false;
    };
    cc::map<i32, custom_call> custom_calls;

    /// The call `id` of `record.callee`, which has a derivative written by hand, inlined over locals that hold its
    /// arguments; the derivative takes those locals rather than the body.
    flat_expr_id flatten_custom_call(ast::expr_id id,
                                     type_id type,
                                     call_record const& record,
                                     custom_derivatives d,
                                     cc::span<flat_expr_id const> values);

    /// What the tree is written for: the compile's target, and none for a test, which the interpreter runs (CHK-426).
    [[nodiscard]] compile_target compiled_for() const { return is_test ? compile_target() : c.compiled_target; }
    /// `target_api` and `target_language` of the prelude, which a compile stands for (CHK-426): the position of the case
    /// the compile names, or -1 for every other symbol.
    [[nodiscard]] i32 target_case_of(symbol_id constant) const;
    /// `has(f)` of the prelude, which the device this compile writes for answers (CHK-426).
    [[nodiscard]] bool is_has_call(symbol_id callee) const;
    /// A `has` call as the literal its device makes of it; its feature must be a constant once inlined.
    flat_expr_id flatten_has(ast::expr_id id, type_id type, call_record const& record);
    /// What a condition of the current file grants the branch it guards: the features of the `has` calls it is, or the
    /// `and` of, that this compile's device has (CHK-426).
    [[nodiscard]] feature_set grants_of(ast::expr_id condition);
    /// What `make()` flattens, with the features `condition` grants added to `granted` while it does (CHK-426).
    template <class F>
    auto granting(ast::expr_id condition, F&& make)
    {
        auto const outer = granted;
        granted |= grants_of(condition);
        auto result = make();
        granted = outer;
        return result;
    }
    /// EMIT-264: what a side a constant does not take consults, which is a variant of the entry all the same.
    /// `flatten_untaken` flattens that side on a copy of this flattener, which is dropped but for what it consulted, so
    /// nothing of it reaches the entry and nothing judges it (CHK-356).
    /// A side is probed once for each place it is inlined at, however often that place repeats, such as in an unrolled
    /// loop: `node` is the side's first node in the current file, which with the call chain names it.
    template <class F>
    void probe_consults(u32 node, F&& flatten_untaken)
    {
        auto key = cc::format("{}:{}", file(), node);
        for (auto const& site : entry.at(current()->chain))
            key.appendf("<{}:{}", site.file, ast::index_of(site.call));
        if (!probed.is_valid())
            probed = cc::make_shared<cc::set<cc::string>>();
        if (probed->contains(key))
            return;
        probed->insert(key);
        auto copy = *this;
        flatten_untaken(copy);
        entry.consulted |= copy.entry.consulted;
    }
    /// The sides probed so far, shared by this flattener and the copies its probes make.
    cc::shared_ptr<cc::set<cc::string>> probed;
    /// EMIT-264: what the conditions of the preambles a body in target text `text` brings consult (CHK-528).
    void consult_preambles_of(i32 text);
    /// The case of a feature an argument names as a constant, `.subgroups`; -1 where it names none.
    [[nodiscard]] i32 feature_case_of(ast::expr_id argument);
    /// Refuses what a compile stands for inside a const, whose value one check computes for every compile (CHK-426).
    /// True where it refused, which makes the flattening fail.
    bool refuses_target_in_const(cc::string_view what);

    /// `by_target(native, emulated)` of raytracing.sgl: both lambdas written out, each a block, and one node holding both.
    [[nodiscard]] bool is_by_target(symbol_id callee) const;

    flat_expr_id flatten_by_target(ast::expr_id id, type_id type, call_record const& record);

    /// The closure `handed`, called with nothing, as a block that leaves with its value.
    flat_expr_id closure_block(ast::expr_id id, type_id type, i32 handed, cc::string_view name);

    /// The closure `handed`, inlined over `values`, which `slots` assigns to its parameters.
    flat_expr_id call_closure(ast::expr_id id,
                              type_id type,
                              i32 handed,
                              cc::span<flat_expr_id const> values,
                              cc::span<i32 const> slots,
                              cc::span<i32 const> argument_closures);

    /// `any_hit` for a candidate, `intersection` for a box: what a decision of that parameter is to the traversal.
    [[nodiscard]] stage decision_of(type_id type) const;

    /// `a < b <= c` is `a < b and b <= c` with `b` evaluated once, and the `and` keeps `c` unevaluated where it must.
    flat_expr_id flatten_chain(ast::expr_id id, type_id type, ast::comparison_chain const& chain);

    /// A `loop:` somebody reads the value of: a block around the loop, which a `break value` leaves.
    flat_expr_id flatten_value_loop(ast::expr_id id, type_id type, ast::loop_expr const& loop);

    /// The scrutinee, the arms and the default of a `case`; `value_block` is the block an arm's value leaves.
    flat_case flatten_case_parts(ast::case_expr const& node, label_id value_block);

    /// The `==` that compares the scrutinee with a pattern; an enum compares as the `int` its cases are.
    symbol_id equality_for(flat_expr_id scrutinee) const;

    /// The prelude's `bool`, even where the user file shadows the name.
    [[nodiscard]] type_id bool_type() const;

    // ---- checks -----------------------------------------------------------------------------------------------------

    /// A check of a test or an `assert`: every node of `condition` is materialized into a `var` of its own, in the order
    /// and under the conditions the plain expression would run them, and the report reads those `var`s (CHK-229).
    void flatten_check(origin from, ast::expr_id condition, bool stops, ast::expr_id message = ast::expr_id::none);
    /// CHK-424: the text of quoted literal `message` around its interpolations, added to `entry.messages`; its position.
    [[nodiscard]] i32 add_message(ast::expr_id message);
    /// Each interpolation of quoted literal `message`, flattened where it stands, in source order.
    [[nodiscard]] ast::range_of<flat_expr_id> flatten_message_values(ast::expr_id message);

    /// A `var` for node `node` of a check, declared without a value where the node's statements begin.
    local_id check_var(ast::expr_id id);

    void assign_check(local_id local, ast::expr_id from, flat_expr_id value);

    /// A node that is only a value.
    local_id check_leaf(ast::expr_id id, i32 parent, cc::vector<flat_check_node>& nodes);

    /// `callee` called with the values of two nodes, as the comparison the check pass resolved.
    flat_expr_id compare_call(ast::expr_id id, symbol_id callee, local_id lhs, local_id rhs);

    [[nodiscard]] static bool is_comparison(cc::string_view op);

    local_id materialize(ast::expr_id id, i32 parent, cc::vector<flat_check_node>& nodes);

    /// `a < b <= c`: each link is a comparison node, and link i + 1 and its new operand run only where link i held.
    local_id materialize_chain(ast::expr_id id,
                               ast::comparison_chain const& chain,
                               i32 parent,
                               cc::vector<flat_check_node>& nodes);

    /// The prelude's `int`, which an enum's comparison runs on, even where the user file shadows the name.
    type_id int_type() const;

    /// `a or b` is a list of patterns rather than the `or` of the language (CHK-157).
    void collect_patterns(ast::expr_id id, cc::vector<flat_expr_id>& into);

    /// The body of a `case` arm or of a branch of an `if` value; `value_block` is what its value leaves, or none.
    ast::range_of<flat_stmt_id> flatten_arm_body(ast::body const& result, label_id value_block);

    /// A `case` somebody reads the value of: a block around it, which every arm leaves.
    flat_expr_id flatten_value_case(ast::expr_id id, type_id type, ast::case_expr const& node);

    /// `if c => a else b` somebody reads the value of: a block around an `if` whose branches leave it (CHK-375).
    /// Only the branch the condition takes runs.
    /// A branch CHK-356 resolves is the taken side alone, and an arrow side that is a value is its expression.
    flat_expr_id flatten_value_if(ast::expr_id id, type_id type, ast::if_expr const& node);

    /// The value of the side of an `if` value that a constant takes: an arrow's expression, or a block it leaves.
    flat_expr_id taken_value(ast::expr_id id, type_id type, ast::body const& then);

    /// CHK-356: the value of `condition` where it is a constant, which a branch on it is replaced by the taken side of.
    /// Nothing for a condition that is no constant, and for one with no value, which `judge_constants` reports.
    [[nodiscard]] cc::optional<bool> constant_condition(flat_expr_id condition) const;

    /// The taken side of a branch on a constant, as a block of its own, so its locals stay in its scope.
    void add_taken(origin from, ast::range_of<flat_stmt_id> body);

    /// The branches of an `if` value, each an `if` nested in the `else` of the one before; the last has no condition.
    /// `first`, where valid, is the first condition flattened already.
    void flatten_if_branches(origin from,
                             cc::span<ast::if_branch const> branches,
                             label_id value_block,
                             flat_expr_id first = flat_expr_id::none);

    // ---- calls ------------------------------------------------------------------------------------------------------

    struct inlined_body
    {
        label_id label = label_id::none;
        ast::range_of<flat_stmt_id> body;
    };

    /// The call sites a node inlined through `call` stands under: the current frame's, then `call`.
    ast::range_of<call_site> chain_through(ast::expr_id call);

    /// Binds a value to parameter `p` in the current frame: a literal or an immutable local stands for it, and
    /// anything else is bound once by a `let` of the block being written.
    /// `keeps_constructions` binds a construction of literals as it stands, for a builtin that takes one only so.
    void bind_parameter(parameter const& p, flat_expr_id value, bool keeps_constructions = false);

    /// The default of every parameter `slots` leaves unfilled, flattened in the callee's frame, which is the current
    /// one, and bound in parameter order (EVAL-80, EVAL-81).
    void bind_defaults(cc::span<parameter const> parameters, cc::span<i32 const> slots);

    /// `inline_bound` for arguments that fill the parameters in order, one each.
    inlined_body inline_call(ast::expr_id call, symbol_id callee, cc::span<flat_expr_id const> arguments);

    /// The body of `callee` as the statements of a block named after it.
    /// `values` were written in the caller's frame, in the order the call wrote them, and are bound at the top of the
    /// block in that order; `slots` says which parameter each one fills.
    inlined_body inline_bound(ast::expr_id call,
                              symbol_id callee,
                              cc::span<flat_expr_id const> values,
                              cc::span<i32 const> slots,
                              cc::span<i32 const> handed = {},
                              cc::span<type_id const> type_arguments = {});

    // ---- statements -------------------------------------------------------------------------------------------------

    ast::range_of<flat_stmt_id> flatten_body(ast::body const& body);

    void flatten_return(origin from, ast::expr_id value);

    /// The run of tagged strings `stmt` opens, as the call of its function that the emitter writes as the text of the
    /// language this compile writes; nothing for a statement that opens no run (CHK-427).
    void flatten_target_text(origin from, ast::stmt_id stmt);

    /// `is_const` for a `const if`, whose condition that no constant decides is an error (CHK-425).
    void flatten_if(origin from, cc::span<ast::if_branch const> branches, bool is_const = false);
    /// CHK-432: each text type the tree holds a value of, as this compile spells it, into `entry.text_types`.
    /// A field or a constructor of one that the compile spells in its target's text is reported.
    void judge_text_types();
    /// CHK-522: every struct with a target layout the tree holds as the struct of the branch this compile takes, its
    /// fields renumbered to that order; one no branch of holds on this compile is reported.
    void place_target_layouts();

    void flatten_assign(origin from, ast::assign_stmt const& assign);

    /// CHK-367: `img[xy] = v` is the `store` the check pass resolved, and `img[xy] op= v` loads the texel first.
    /// A compound one evaluates the image's index and the coordinates once, which the load and the store both read.
    void flatten_texel_store(origin from, ast::assign_stmt const& assign, texel_store const& t);

    /// What `place op= value` reads the place as, which is the place read a second time.
    /// A local or a member of one holds nothing to evaluate, so it is simply flattened again.
    /// A buffer element's index is evaluated once (EVAL-14): the place gets its first evaluation, and the read the local
    /// that holds it.
    flat_expr_id read_of_place(flat_expr_id place, ast::expr_id target);

    [[nodiscard]] bool has_array_index(flat_expr_id id) const;

    /// The place `id` read a second time, each array index in it evaluated once: the place keeps the first evaluation,
    /// and the read takes the local that holds it (EVAL-14).
    flat_expr_id reread(flat_expr_id id, ast::expr_id from);

    /// The most copies of its body an unrolled `for` writes, each counted once per copy of the unrolled loops around it (CHK-389).
    static constexpr i64 k_max_unrolled = 256;

    /// The value of constant `bound`, an int, where it is one; nothing for anything else.
    /// The innermost call the statement being written was inlined through, none in the tree's own body.
    [[nodiscard]] call_site innermost_call() const;

    [[nodiscard]] cc::optional<i32> constant_int(flat_expr_id bound) const;

    /// The label a run of `id` certainly leaves to, `none` for a return, where no run of it reaches its end.
    /// Nothing where a run may reach its end, or where two of them leave to different places.
    [[nodiscard]] cc::optional<label_id> certain_exit(flat_stmt_id id) const;

    [[nodiscard]] cc::optional<label_id> certain_exit(ast::range_of<flat_stmt_id> body) const;

    /// CHK-389: `@unroll for i in a ..< b`, as one block holding a block per index, each a copy of the body in which
    /// `i` is that index's literal.
    /// A `break` leaves the outer block and a `continue` the copy it stands in, which is what they do in the loop.
    /// A range whose copies would pass the limit is the loop it would be without the attribute, and a warning says so.
    void flatten_unrolled_for(origin from, ast::stmt_id id, ast::for_stmt const& loop, flat_expr_id first, flat_expr_id end);

    /// An unrolled `for` written as a loop, which the unrolled `for`s inside it see.
    void flatten_fallen_back_for(origin from,
                                 ast::stmt_id id,
                                 ast::for_stmt const& loop,
                                 flat_expr_id first,
                                 flat_expr_id end);

    /// `@unroll(false)` on the loop `id`, which stays a loop and asks its target to keep it one (CHK-414).
    [[nodiscard]] bool is_rolled(ast::stmt_id id) const;

    /// `@unroll` on the loop `id`, written without `false` (CHK-389).
    [[nodiscard]] bool is_unrolled(ast::stmt_id id) const;

    /// `for i in a ..< b` as a loop of the flat tree, whose bounds `first` and `end` are flattened already.
    void flatten_looped_for(origin from, ast::stmt_id id, ast::for_stmt const& loop, flat_expr_id first, flat_expr_id end);

    /// `for i in a ..= b`: the `for` over `a ..< b + 1` where `b` is a constant below the largest int, and otherwise a
    /// loop that tests after its body, so `b + 1` is never computed and a range up to the largest int runs whole.
    /// A `continue` leaves the body's block and reaches the test, as it reaches the next index of a `for`.
    void flatten_inclusive_for(origin from,
                               ast::stmt_id id,
                               ast::for_stmt const& loop,
                               flat_expr_id first,
                               flat_expr_id last);

    /// The walk set for the `for` being flattened, which no `for` inside its body sees.
    [[nodiscard]] cc::optional<walk_mapping> take_walk();

    /// `start + k * step`, folded where its parts are constants, so a walk over a constant range reads as one.
    flat_expr_id walked(walk_mapping const& walk, flat_expr_id k, ast::expr_id where);

    /// CHK-415: a range walked in reverse or in steps, as a `for` of `k` over `0 ..< count` whose variable is the
    /// walk's `k`-th value; every part is an int, its bounds evaluated once, first before last.
    void flatten_walked_for(origin from, ast::stmt_id id, ast::for_stmt const& loop, checker::range_walk const& walk);

    void flatten_for(origin from, ast::stmt_id id, ast::for_stmt const& loop);

    void flatten_stmt(ast::stmt_id id);

    void flatten_expr_stmt(origin from, ast::expr_id value);

    /// Far beyond any program; it bounds the work on a tree the check pass should never have let through.
    static constexpr isize k_max_inline_depth = 256;
};
} // namespace sgl::check::impl
