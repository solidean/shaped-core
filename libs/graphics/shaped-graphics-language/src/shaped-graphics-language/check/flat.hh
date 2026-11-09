#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/builtins/ids.hh>
#include <shaped-graphics-language/check/ids.hh>
#include <shaped-graphics-language/check/symbols.hh>

/// The flat typed tree of one entry point, which is all an emitter reads besides the module's types and bindings.
///
/// Full inlining is the language's model, so nothing here is a user function, a generic or a lambda.
/// What is left: locals, structured control flow, member access, constructions, literals and calls of builtins.
/// Every node keeps the AST node it came from and the chain of call sites it was inlined through.
///
/// One data structure holds two forms.
/// The STRUCTURED form is what the check pass writes: labeled blocks that may be expressions, and `leave` from any depth.
/// The CORE form is the subset every target prints one to one, and `find_core_violation` (legalize/core.hh) defines it.
/// `legalize` (legalize/legalize.hh) takes the first to the second, and `interpret` (interpret/interpret.hh) runs both.

namespace sgl::check
{
template <class T>
struct tree_view;
}

/// A view of one of the flat tree's side arrays.
///
/// Every pass reads the tree and appends to it at the same time, so an id stays valid and a VIEW does not:
/// the array behind it can move, and what was a span then points at memory nobody owns.
/// Outside a release build a view remembers the array it was made from and checks, on every element it hands out,
/// that the array has not moved since — so one held across an append fails loudly instead of reading freed memory.
///
/// The fix at a call site is always the same: copy the list into a `cc::vector` before writing into the tree.
template <class T>
struct sgl::check::tree_view
{
    T const* items = nullptr;
    isize count = 0;
#if CC_ASSERT_ENABLED
    /// The array this view was made from, and where it kept its elements then.
    cc::vector<T> const* owner = nullptr;
    T const* base = nullptr;
#endif

    void verify() const
    {
#if CC_ASSERT_ENABLED
        CC_ASSERT(owner == nullptr || owner->data() == base, "a view of the flat tree outlived an append to it: copy "
                                                             "the list before writing into the tree");
#endif
    }

    /// Indexed rather than a pointer, so every element goes through `verify`.
    struct iterator
    {
        tree_view const* view = nullptr;
        isize at = 0;

        [[nodiscard]] T const& operator*() const { return (*view)[at]; }
        iterator& operator++()
        {
            ++at;
            return *this;
        }
        [[nodiscard]] bool operator!=(iterator const& rhs) const { return at != rhs.at; }
    };

    [[nodiscard]] isize size() const { return count; }
    [[nodiscard]] bool empty() const { return count == 0; }
    [[nodiscard]] T const* data() const
    {
        verify();
        return items;
    }
    [[nodiscard]] T const& operator[](isize i) const
    {
        verify();
        return items[i];
    }
    [[nodiscard]] T const& front() const { return (*this)[0]; }
    [[nodiscard]] T const& back() const { return (*this)[count - 1]; }
    [[nodiscard]] iterator begin() const { return {this, 0}; }
    [[nodiscard]] iterator end() const { return {this, count}; }
    operator cc::span<T const>() const { return {data(), count}; }
};

namespace sgl::check
{
/// `r` of `owner`, as a view that notices the array moving under it.
template <class T>
[[nodiscard]] tree_view<T> viewed(cc::vector<T> const& owner, ast::range_of<T> r)
{
    CC_ASSERT(isize(r.first) + isize(r.count) <= owner.size(), "a range that reaches outside the tree");
    auto view = tree_view<T>{.items = owner.data() + isize(r.first), .count = isize(r.count)};
#if CC_ASSERT_ENABLED
    view.owner = &owner;
    view.base = owner.data();
#endif
    return view;
}
} // namespace sgl::check

/// The one place a name an emitter writes comes from.
/// A minted name is free, so a collision is impossible by construction; the check pass mints the locals and an
/// emitter goes on minting from the same value.
struct sgl::check::name_mint
{
    /// Every name handed out or reserved so far, in that order, and the same names as a set to look them up in.
    /// Both are written by `reserve` and `mint` alone.
    cc::vector<cc::string> taken;
    cc::set<cc::string> index;
    /// Names taken outside this mint, which it never hands out and never writes: a mint it goes on from, and a set such as every name a builtin writes.
    /// Whoever owns either must outlive this mint and leave it unchanged meanwhile.
    /// What they hold is in neither `taken` nor `index`, so a mint that goes on from another copies none of its names.
    name_mint const* parent = nullptr;
    cc::set<cc::string> const* also_taken = nullptr;
    /// A lower layer's names, such as the prelude's every module-level name, which every program behind it shares.
    cc::set<cc::string> const* also_taken_below = nullptr;
    /// A target's reserved words, which no name of the program written there may be.
    cc::set<cc::string> const* also_reserved = nullptr;
    /// For each name `mint` found taken, the suffix it tries first the next time it is asked for that name.
    /// A name once taken stays taken, so the first free suffix only grows, and minting a name a thousand times is linear.
    cc::map<cc::string, i32> next_suffix;

    [[nodiscard]] bool is_taken(cc::string_view name) const;

    /// Takes `name` as it is, for a name that must not change: an entry point, a type, a target's keyword.
    /// Returns false when it was taken already, which is the caller's error to report.
    bool reserve(cc::string_view name);

    /// `desired` when it is free, else `desired_1`, `desired_2`, … and the first of them that is; the result is taken.
    /// An empty `desired` mints from `_`.
    [[nodiscard]] cc::string mint(cc::string_view desired);

    [[nodiscard]] bool operator==(name_mint const& rhs) const { return ast::impl::is_equal(taken, rhs.taken); }
};

/// The AST node a flat node came from.
struct sgl::check::origin
{
    i32 file = 0;
    /// Exactly one of the two is valid, by whether the flat node is an expression or a statement.
    ast::expr_id expr = ast::expr_id::none;
    ast::stmt_id stmt = ast::stmt_id::none;

    constexpr bool operator==(origin const&) const = default;
};

/// One call a flat node was inlined through, outermost first.
struct sgl::check::call_site
{
    i32 file = 0;
    ast::expr_id call = ast::expr_id::none;

    constexpr bool operator==(call_site const&) const = default;
};

enum class sgl::check::local_kind : sgl::u8
{
    /// The entry point's own parameter, which an emitter declares in the signature.
    parameter,
    /// Introduced by a `let`, or by a parameter of an inlined call whose argument had to be bound.
    let,
    /// Introduced by a `let mut`: mutable, and the only kind of the program's own that an assignment may name.
    var,
    /// The counter of a `for`, which its loop declares and which no statement assigns.
    index,
    /// Introduced by a pass to evaluate something once or to carry a result: a splatted value, a pin, a flag.
    temporary,
};

struct sgl::check::flat_local
{
    local_kind kind = local_kind::let;
    /// Minted, so it is unique within the entry point and differs from every module-level name.
    cc::string name;
    type_id type = type_id::none;
    bool is_mut = false;
    /// The prelude's `undefined()`: never assigned, and handed on and stored as a value whose content means nothing.
    bool is_undefined = false;
    /// LEGAL-68: declared with no value, and reading as a zero of its type.
    bool is_zeroed = false;

    bool operator==(flat_local const&) const = default;
};

/// The name of a block or a loop, which a `leave` or a `continue` refers to.
/// No target writes it: it is for a dump, and the legalizer names a flag after it.
struct sgl::check::flat_label
{
    cc::string name;
    /// A loop written `@unroll(false)`, which the target is asked to keep a loop; a hint, which HLSL writes as `[loop]`
    /// and the other targets cannot spell (CHK-414).
    bool is_rolled = false;

    bool operator==(flat_label const&) const = default;
};

/// Never part of a finished entry point; it is what a node holds before it is filled.
struct sgl::check::flat_invalid
{
    constexpr bool operator==(flat_invalid const&) const = default;
};

/// A float literal; the origin has its spelling.
struct sgl::check::flat_literal
{
    f64 value = 0;

    constexpr bool operator==(flat_literal const&) const = default;
};

/// Of the prelude's type `int`, or of `uint` where an integer literal converted to it (CHK-253); a `uint` keeps its
/// bits in `value`.
struct sgl::check::flat_int_literal
{
    i32 value = 0;
    bool is_unsigned = false;
    /// Written in hex, which the targets keep where the value is no negative int: a bit pattern reads as one only there.
    bool is_hex = false;

    constexpr bool operator==(flat_int_literal const&) const = default;
};

/// Of `long` or `ulong`, its 64 bits two's complement either way (CHK-450).
struct sgl::check::flat_wide_literal
{
    u64 bits = 0;

    constexpr bool operator==(flat_wide_literal const&) const = default;
};

/// Of the prelude's type `bool`.
struct sgl::check::flat_bool_literal
{
    bool value = false;

    constexpr bool operator==(flat_bool_literal const&) const = default;
};

/// One case of an enum, whose type is the enum: a value a target writes as the constant of EMIT-76.
/// The case rather than the value, since two cases may hold one value and only the case names a constant.
struct sgl::check::flat_enum_value
{
    /// A position in the `cases` of the node's enum type.
    i32 case_index = -1;

    constexpr bool operator==(flat_enum_value const&) const = default;
};

struct sgl::check::flat_local_ref
{
    local_id local = local_id::none;

    constexpr bool operator==(flat_local_ref const&) const = default;
};

/// `constants.view_projection`: a member of a binding, which is a global of the target.
struct sgl::check::flat_binding_member
{
    symbol_id binding = symbol_id::none;
    /// A position in the binding's `members`.
    i32 member = -1;
    /// Of a `@workgroup` binding: memory the workgroup shares, which a store changes between two reads of it.
    bool is_workgroup = false;

    constexpr bool operator==(flat_binding_member const&) const = default;
};

/// A file-scope sampler, which is a global of the target at the pipeline layout's address for it (CHK-314).
struct sgl::check::flat_file_sampler
{
    /// Of kind `sampler`.
    symbol_id sampler = symbol_id::none;

    constexpr bool operator==(flat_file_sampler const&) const = default;
};

/// `v.normal`, one field of a struct; or `v.zyx`, a swizzle of a prelude vector, whose value is the plain vector of
/// its fields (CHK-385).
/// A swizzle of a struct of the program never stands here: the check pass writes it as the construction it means.
struct sgl::check::flat_member
{
    flat_expr_id object = flat_expr_id::none;
    /// A position in the `members` of the object's struct type; -1 for a swizzle.
    i32 member = -1;
    /// The fields a swizzle reads, two to four of them; a `count` of zero for one field.
    swizzle letters;

    [[nodiscard]] constexpr bool is_swizzle() const { return letters.count > 0; }

    constexpr bool operator==(flat_member const&) const = default;
};

/// `values[i]` on a `buffer`: the element a shader loads, and the place it stores to.
/// `buffer` is what names the resource: a `flat_binding_member`, or a `flat_element` of a binding array of them.
/// As a place, `index` is evaluated before the value that is stored.
struct sgl::check::flat_buffer_element
{
    flat_expr_id buffer = flat_expr_id::none;
    flat_expr_id index = flat_expr_id::none;

    constexpr bool operator==(flat_buffer_element const&) const = default;
};

/// `values.length` of a buffer or of `bytes`: how many elements, or bytes, the bound memory holds (CHK-460).
/// `buffer` is a `flat_binding_member`, or a `flat_element` of a binding array of them.
struct sgl::check::flat_buffer_length
{
    flat_expr_id buffer = flat_expr_id::none;

    constexpr bool operator==(flat_buffer_length const&) const = default;
};

/// Word `word` of the shadow of workgroup memory (LEGAL-69), an `atomic[uint]` place that only an atomic's call reaches.
/// Only the last step of legalize writes it, so no pass between sees one, and each target declares the shadow beside
/// that memory, `written_words` long.
struct sgl::check::flat_written_word
{
    flat_expr_id word = flat_expr_id::none;

    constexpr bool operator==(flat_written_word const&) const = default;
};

/// `object[index]` on an array value: its element, read or, as a place, assigned.
/// The index is evaluated after the object; outside `0 ..< length` it is a program error (EVAL-90).
struct sgl::check::flat_element
{
    flat_expr_id object = flat_expr_id::none;
    flat_expr_id index = flat_expr_id::none;

    constexpr bool operator==(flat_element const&) const = default;
};

/// A value of the node's struct type from one value per field, in field order, or of its array type, one per element.
/// A splat is gone: its fields stand here one by one.
struct sgl::check::flat_construct
{
    ast::range_of<flat_expr_id> arguments;

    constexpr bool operator==(flat_construct const&) const = default;
};

/// A call of a `@builtin` function, operators included; no other call survives inlining.
struct sgl::check::flat_call
{
    symbol_id callee = symbol_id::none;
    /// The callee's `symbol::intrinsic`: the registry record an emitter and the interpreter look up.
    builtin_id intrinsic = builtin_id::none;
    /// The callee's `function_info::is_pure`, repeated for the same reason; a call that is not pure has an effect.
    bool is_pure = false;
    ast::range_of<flat_expr_id> arguments;
    /// A body in a target's own text, as a position in `checked_module::target_texts`, which the emitter writes in place
    /// of the call; -1 for a builtin's call (CHK-427).
    /// Its callee is the SGL function, its arguments that function's parameters in order, and it has no `intrinsic`.
    i32 text = -1;

    constexpr bool operator==(flat_call const&) const = default;
};

struct sgl::check::flat_not
{
    flat_expr_id operand = flat_expr_id::none;

    constexpr bool operator==(flat_not const&) const = default;
};

/// Short-circuit: `rhs` is evaluated only when `lhs` is true.
struct sgl::check::flat_and
{
    flat_expr_id lhs = flat_expr_id::none;
    flat_expr_id rhs = flat_expr_id::none;

    constexpr bool operator==(flat_and const&) const = default;
};

/// Short-circuit: `rhs` is evaluated only when `lhs` is false.
struct sgl::check::flat_or
{
    flat_expr_id lhs = flat_expr_id::none;
    flat_expr_id rhs = flat_expr_id::none;

    constexpr bool operator==(flat_or const&) const = default;
};

/// `block $label { … }`, the one construct an inlined call, a value block and a `loop:` with a value all become.
/// As a statement it has no value, and as an expression its value is what a `leave $label value` gives.
/// Structured form only.
struct sgl::check::flat_block
{
    label_id label = label_id::none;
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_block const&) const = default;
};

/// The two forms of one value, of which a target writes exactly one: the native form where the device has the construct,
/// the emulated one where sg does it in software (the trace of raytracing.sgl).
/// Each is a `flat_block` expression.
/// Structured form only: legalize keeps one of the two by `legalize_options`.
struct sgl::check::flat_by_target
{
    flat_expr_id native = flat_expr_id::none;
    flat_expr_id emulated = flat_expr_id::none;

    constexpr bool operator==(flat_by_target const&) const = default;
};

struct sgl::check::flat_expr
{
    type_id type = type_id::none;
    origin from;
    /// A range of `flat_entry_point::call_sites`; empty for a node of the entry point's own body.
    ast::range_of<call_site> inlined_through;

    cc::variant<flat_invalid,
                flat_literal,
                flat_int_literal,
                flat_wide_literal,
                flat_bool_literal,
                flat_enum_value,
                flat_local_ref,
                flat_binding_member,
                flat_file_sampler,
                flat_member,
                flat_buffer_element,
                flat_buffer_length,
                flat_element,
                flat_written_word,
                flat_construct,
                flat_call,
                flat_not,
                flat_and,
                flat_or,
                flat_block,
                flat_by_target>
        node;

    bool operator==(flat_expr const&) const = default;
};

/// Declares `local` and gives it its value.
struct sgl::check::flat_let
{
    local_id local = local_id::none;
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_let const&) const = default;
};

/// Declares the mutable `local`; without a `value` it holds nothing until it is assigned.
struct sgl::check::flat_var
{
    local_id local = local_id::none;
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_var const&) const = default;
};

/// `place` is a `flat_local_ref` of a mutable local, a chain of `flat_member` over one, or a `flat_buffer_element` of a
/// `mut` buffer, whose index is evaluated before `value`.
/// A swizzle stands only on top of a place, its fields distinct; over a buffer element its index is a local or a literal.
struct sgl::check::flat_assign
{
    flat_expr_id place = flat_expr_id::none;
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_assign const&) const = default;
};

/// The text of a `print`'s or an `assert`'s message around its interpolations (CHK-424).
struct sgl::check::flat_message
{
    /// One more than the interpolations: the text before each, then the text after the last, its escapes resolved.
    cc::vector<cc::string> pieces;
    /// An `assert`'s, which a print reports only where its condition was false (LEGAL-64).
    bool is_assert = false;

    bool operator==(flat_message const& rhs) const
    {
        return ast::impl::is_equal(pieces, rhs.pieces) && is_assert == rhs.is_assert;
    }
};

/// Records `values`, which are the one effect a program has besides its result.
/// No emitter writes it yet.
struct sgl::check::flat_print
{
    /// A position in `flat_entry_point::messages`; -1 for a print of bare values, which only a built tree holds.
    i32 message = -1;
    ast::range_of<flat_expr_id> values;

    constexpr bool operator==(flat_print const&) const = default;
};

/// Evaluates `value` and drops it: a call somebody wrote for its effect alone.
struct sgl::check::flat_eval
{
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_eval const&) const = default;
};

struct sgl::check::flat_if
{
    flat_expr_id condition = flat_expr_id::none;
    ast::range_of<flat_stmt_id> then_body;
    ast::range_of<flat_stmt_id> else_body;

    constexpr bool operator==(flat_if const&) const = default;
};

/// Exits the block or the loop `target` from any depth inside it; `value` is what a block expression then is.
/// `return`, `yield` and `break value` of the source all become this, and `leave $root value` is the function's return.
/// Structured form only.
struct sgl::check::flat_leave
{
    label_id target = label_id::none;
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_leave const&) const = default;
};

/// Runs `body` until something exits it.
struct sgl::check::flat_loop
{
    label_id label = label_id::none;
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_loop const&) const = default;
};

/// `condition` is evaluated before every iteration, the one after a `continue` included.
struct sgl::check::flat_while
{
    label_id label = label_id::none;
    flat_expr_id condition = flat_expr_id::none;
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_while const&) const = default;
};

/// `for index in first ..< end`: both bounds are evaluated ONCE, `first` before `end`, before the first iteration.
/// `index` is an `int` local of kind `index`, which holds a fresh value in every iteration.
struct sgl::check::flat_for
{
    label_id label = label_id::none;
    local_id index = local_id::none;
    flat_expr_id first = flat_expr_id::none;
    flat_expr_id end = flat_expr_id::none;
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_for const&) const = default;
};

/// Ends the invocation with no effect: nothing after it runs, and what it would have written is never written.
/// A pixel entry point's alone (CHK-277); a test's run ends as `discarded`.
struct sgl::check::flat_discard
{
    constexpr bool operator==(flat_discard const&) const = default;
};

/// Starts the next iteration of the loop `target`, from any depth inside it in the structured form.
/// In the core form `target` is the innermost loop, and no `once` stands in between.
struct sgl::check::flat_continue
{
    label_id target = label_id::none;

    constexpr bool operator==(flat_continue const&) const = default;
};

/// Runs `body` once; a `break` directly inside it exits it.
/// Core form only: `do { … } while (false);` in the C-like targets.
struct sgl::check::flat_once
{
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_once const&) const = default;
};

/// Exits the innermost enclosing `once` or loop.
/// Core form only.
struct sgl::check::flat_break
{
    constexpr bool operator==(flat_break const&) const = default;
};

/// Leaves the entry point with its result, from any depth.
/// Core form; in a structured tree it means `leave $root value`, and it is how the check pass spells the entry point's own `return`.
struct sgl::check::flat_return
{
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_return const&) const = default;
};

/// What one node of a check's condition is: the operators a report narrows through, and the leaf any other expression is.
enum class sgl::check::check_node_kind : sgl::u8
{
    leaf,
    and_,
    or_,
    not_,
    /// One comparison, whose operands are the nodes `lhs` and `rhs`.
    compare,
    /// `a < b <= c`: an `and` of its comparisons, which share the operands between them.
    chain,
};

/// One node of the condition of a check or an `assert`, in the order the condition is written.
struct sgl::check::flat_check_node
{
    check_node_kind kind = check_node_kind::leaf;
    origin from;
    /// The spelling of a comparison: `<`, `==`.
    cc::string op;
    /// A position among the site's nodes; -1 for the whole condition.
    i32 parent = -1;
    /// For a comparison, its operands as positions among the site's nodes.
    i32 lhs = -1;
    i32 rhs = -1;
    /// The `var` a run leaves this node's value in; it holds none where the node did not run (EVAL-11).
    local_id value = local_id::none;

    bool operator==(flat_check_node const&) const = default;
};

/// What a failing check is reported from: where it stands, and its condition as a tree of nodes.
struct sgl::check::flat_check_site
{
    origin from;
    /// An `assert`, which stops the run where it is false; a check of a test goes on.
    bool stops = false;
    /// A range of `flat_entry_point::check_nodes`; the first is the whole condition.
    ast::range_of<flat_check_node> nodes;
    /// A `flat_local_ref` per `for` of the test around the check, outermost first.
    ast::range_of<flat_expr_id> loop_variables;
    /// An `assert`'s message as a position in `flat_entry_point::messages`, -1 for none, and its interpolated values.
    /// The values are computed by the check's body, beside the condition.
    i32 message = -1;
    ast::range_of<flat_expr_id> message_values;

    constexpr bool operator==(flat_check_site const&) const = default;
};

/// Runs `body`, which leaves the condition's value in the first node's `var`, and records the check when it is false.
/// Structured form only: `legalize` removes it with its body, which is how no target writes a check or an `assert`.
struct sgl::check::flat_check
{
    /// A position in `flat_entry_point::check_sites`.
    i32 site = -1;
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_check const&) const = default;
};

/// One arm: the patterns that select it, and the statements it runs.
/// In a `flat_case` a pattern is any expression; in a `flat_switch` it is a literal the target compares.
struct sgl::check::flat_arm
{
    ast::range_of<flat_expr_id> patterns;
    ast::range_of<flat_stmt_id> body;

    constexpr bool operator==(flat_arm const&) const = default;
};

/// `case v { … default { … } }`: `v` is evaluated once, then the arms are tried in order and the first match runs.
/// A pattern is evaluated only where it is reached (EVAL-68), and the `default` arm runs when no other matched.
/// Structured form only; `legalize` takes it to a `switch` or to a chain of `if`.
struct sgl::check::flat_case
{
    flat_expr_id scrutinee = flat_expr_id::none;
    /// The `==` an arm matches by, which the chain form of LEGAL-46 calls; only the check pass can resolve it.
    /// An enum's is the `int` overload, since the cases' `int`s are what EVAL-64 compares.
    symbol_id equality = symbol_id::none;
    builtin_id equality_intrinsic = builtin_id::none;
    ast::range_of<flat_arm> arms;
    ast::range_of<flat_stmt_id> default_body;

    constexpr bool operator==(flat_case const&) const = default;
};

/// `switch v { [a, b] { … } … default { … } }`, whose every pattern is a literal and whose `default` is mandatory.
/// A `break` directly inside an arm ends the switch, the way one inside a `once` ends that.
/// Core form only.
struct sgl::check::flat_switch
{
    flat_expr_id scrutinee = flat_expr_id::none;
    ast::range_of<flat_arm> arms;
    ast::range_of<flat_stmt_id> default_body;

    constexpr bool operator==(flat_switch const&) const = default;
};

/// A line of an SGL source: a position in `checked_module::files`, and the line there, counted from 1.
struct sgl::check::source_line
{
    /// -1 for no line at all.
    i32 file = -1;
    i32 line = 0;

    constexpr bool operator==(source_line const&) const = default;
};

/// Where one line of the text came from (EMIT-181).
struct sgl::check::line_origin
{
    /// The line of the text, counted from 1.
    i32 emitted = 0;
    /// The innermost source line outside the prelude: a prelude function's body maps to the line that called it.
    source_line at;
    /// The calls `at` was inlined through, innermost first, the prelude's left out; empty for the entry point's own body.
    cc::vector<source_line> inlined_from;

    bool operator==(line_origin const& rhs) const
    {
        return emitted == rhs.emitted && at == rhs.at && ast::impl::is_equal(inlined_from, rhs.inlined_from);
    }
};

/// One value a debug record holds: its scalar family and its lanes, each lane one 32-bit word.
struct sgl::check::debug_value
{
    /// `float`, `half`, `int`, `uint` or `bool`; an enum is `int`, and `type` names it.
    cc::string family;
    i32 lanes = 1;
    /// The value's type as the program names it: `float3`, `material_kind`.
    cc::string type;

    bool operator==(debug_value const&) const = default;
};

/// One print or assert the text writes into sg's debug buffer (EMIT-183).
/// Its record is `id`, the record's word count, then each value's lanes in order, one word each.
struct sgl::check::debug_site
{
    /// What the record starts with, stable for one source line and message wherever the text is written.
    u32 id = 0;
    /// An assert's, which writes only where its condition was false.
    bool is_assert = false;
    /// The message around its values, one piece more than `values`; empty for a print of one bare value.
    cc::vector<cc::string> pieces;
    cc::vector<debug_value> values;
    /// Where the print or assert stands; empty without the source files (EMIT-181).
    line_origin origin;

    bool operator==(debug_site const& rhs) const
    {
        return id == rhs.id && is_assert == rhs.is_assert && ast::impl::is_equal(pieces, rhs.pieces)
            && ast::impl::is_equal(values, rhs.values) && origin == rhs.origin;
    }
};

struct sgl::check::flat_stmt
{
    origin from;
    ast::range_of<call_site> inlined_through;

    cc::variant<flat_let,
                flat_var,
                flat_assign,
                flat_print,
                flat_eval,
                flat_if,
                flat_block,
                flat_leave,
                flat_loop,
                flat_while,
                flat_for,
                flat_continue,
                flat_discard,
                flat_once,
                flat_break,
                flat_case,
                flat_switch,
                flat_return,
                flat_check>
        node;

    bool operator==(flat_stmt const&) const = default;
};

/// A parameter of an entry point that the GPU fills, and the local that holds it.
struct sgl::check::flat_stage_input
{
    stage_input input = stage_input::none;
    local_id local = local_id::none;

    bool operator==(flat_stage_input const&) const = default;
};

/// One entry point as one flat function.
/// A type, a symbol and a binding are ids into the `checked_module` this value stands in.
/// A ray type an entry point traces: its set, and its position there.
struct sgl::check::flat_traced_ray
{
    symbol_id set = symbol_id::none;
    i32 ray = 0;

    constexpr bool operator==(flat_traced_ray const&) const = default;
};

/// A text type this compile spells in its target's own text, and how (CHK-432).
struct sgl::check::flat_text_type
{
    type_id type = type_id::none;
    cc::string spelling;

    bool operator==(flat_text_type const&) const = default;
};

struct sgl::check::flat_entry_point
{
    stage entry_stage = stage::none;
    /// The name as written: the host asks for it, so it is never minted.
    cc::string name;
    symbol_id function = symbol_id::none;
    /// The stage struct, which is `locals[0]`; `none` for an entry point that takes stage inputs alone.
    type_id input = type_id::none;
    /// The parameters that are stage inputs, each a local of its own, in the order written (CHK-271).
    cc::vector<flat_stage_input> stage_inputs;
    type_id result = type_id::none;
    /// The bindings of the function's `{...}` list in the order written, which is what decides the pipeline layout.
    cc::vector<symbol_id> bindings;
    /// The grid a `compute` entry point is dispatched in; `{1, 1, 1}` for every other stage.
    cc::fixed_array<i32, 3> workgroup = {1, 1, 1};
    /// CHK-520: the compile chose `workgroup`, so the host dispatches the entry point by threads alone, and a thread
    /// past the grid sg passes in returns before the body.
    bool is_auto_workgroup = false;
    /// `function_info::preferred_subgroup_size`, which the host asks the device for and no text but dx12's states.
    i32 preferred_subgroup_size = 0;
    /// What a device needs to run it, `function_info::features`.
    feature_set features;
    /// The strictest `@float` of every function it reaches, itself included (CHK-435), and the first function that
    /// stated it; `none` at `fast`.
    float_level float_guarantee = float_level::fast;
    symbol_id float_raised_by = symbol_id::none;
    /// The options it reaches, each once, in declaration order (CHK-355); empty for a test.
    cc::vector<symbol_id> options;
    /// This entry point legalized with the default options, where its check did so already to judge its uniformity.
    /// What `legalize(m, *this)` gives, so a reader that wants that form takes it; null where the check did not.
    cc::shared_ptr<flat_entry_point const> legalized;
    /// The ray types it traces, by their position in their set, each once (CHK-332).
    cc::vector<flat_traced_ray> traced_rays;
    /// The text types it holds a value of that its compile spells in the target's text, each once (CHK-432).
    cc::vector<flat_text_type> text_types;
    /// A procedural hit's attributes, which the target hands a closest or any hit as a parameter of its own; `none`
    /// for every other entry point.
    local_id attributes = local_id::none;
    /// Set where a metal traversal's fused any hit decided `accept_and_end_search`, which the traversal's result then
    /// says (CHK-345); `none` for every other entry point.
    local_id ends_search = local_id::none;
    /// LEGAL-69: the 32-bit words of the shadow that says which scalars of workgroup memory were stored, which every
    /// target declares (EMIT-229) and LEGAL-71 clears before the body; 0 where nothing tests it.
    i32 written_words = 0;
    /// LEGAL-69 under `define`: LEGAL-71 zeroes workgroup memory before the body, as WGSL's starts.
    bool zeroes_workgroup = false;
    /// LEGAL-69: the bytes of workgroup memory a profile left unshadowed, since its shadow would not fit beside them in
    /// CHK-294's budget; 0 where every read is tested or nothing asked for it.
    i32 unshadowed_bytes = 0;
    /// The local holding the invocation's index in its workgroup, which that prologue strides by; `none` where there is
    /// none.
    local_id workgroup_index = local_id::none;
    /// The entries of `k_reserved_bindings` the tree reaches, which no `bindings` list holds: legalize adds them.
    cc::vector<symbol_id> reserved;
    /// EMIT-183: each print legalize wrote as a record.
    cc::vector<debug_site> debug_sites;

    cc::vector<flat_local> locals;
    cc::vector<flat_label> labels;
    /// The label of the function body, which a `leave` names to return; `none` for a tree that only has `flat_return`.
    label_id root = label_id::none;
    cc::vector<flat_expr> exprs;
    cc::vector<flat_stmt> stmts;
    cc::vector<flat_expr_id> expr_lists;
    cc::vector<flat_stmt_id> stmt_lists;
    /// The arms of every `case` and `switch` of this entry point.
    cc::vector<flat_arm> arms;
    cc::vector<call_site> call_sites;
    /// Every check and `assert` of the tree, which a `flat_check` names by position; empty once the tree is core.
    cc::vector<flat_check_site> check_sites;
    cc::vector<flat_check_node> check_nodes;
    /// The text of every message a `print` or a check names by position.
    cc::vector<flat_message> messages;
    /// The statements of the function, in order.
    ast::range_of<flat_stmt_id> body;

    /// Holds the entry point's name, every module-level name and every local; an emitter mints the rest from it.
    name_mint names;

    [[nodiscard]] flat_local const& at(local_id id) const { return locals[index_of(id)]; }
    [[nodiscard]] flat_label const& at(label_id id) const { return labels[index_of(id)]; }
    [[nodiscard]] flat_expr const& at(flat_expr_id id) const { return exprs[index_of(id)]; }
    [[nodiscard]] flat_stmt const& at(flat_stmt_id id) const { return stmts[index_of(id)]; }
    [[nodiscard]] tree_view<flat_expr_id> at(ast::range_of<flat_expr_id> r) const { return viewed(expr_lists, r); }
    [[nodiscard]] tree_view<flat_stmt_id> at(ast::range_of<flat_stmt_id> r) const { return viewed(stmt_lists, r); }
    [[nodiscard]] tree_view<flat_arm> at(ast::range_of<flat_arm> r) const { return viewed(arms, r); }
    [[nodiscard]] tree_view<call_site> at(ast::range_of<call_site> r) const { return viewed(call_sites, r); }
    [[nodiscard]] tree_view<flat_check_node> at(ast::range_of<flat_check_node> r) const
    {
        return viewed(check_nodes, r);
    }

    [[nodiscard]] bool operator==(flat_entry_point const& rhs) const
    {
        using ast::impl::is_equal;
        return entry_stage == rhs.entry_stage && name == rhs.name && function == rhs.function && input == rhs.input
            && result == rhs.result && is_equal(bindings, rhs.bindings) && workgroup[0] == rhs.workgroup[0]
            && workgroup[1] == rhs.workgroup[1] && workgroup[2] == rhs.workgroup[2]
            && is_auto_workgroup == rhs.is_auto_workgroup && preferred_subgroup_size == rhs.preferred_subgroup_size
            && is_equal(stage_inputs, rhs.stage_inputs) && is_equal(locals, rhs.locals) && is_equal(labels, rhs.labels)
            && root == rhs.root && is_equal(exprs, rhs.exprs) && is_equal(stmts, rhs.stmts)
            && is_equal(expr_lists, rhs.expr_lists) && is_equal(stmt_lists, rhs.stmt_lists) && is_equal(arms, rhs.arms)
            && is_equal(call_sites, rhs.call_sites) && is_equal(check_sites, rhs.check_sites)
            && is_equal(check_nodes, rhs.check_nodes) && is_equal(messages, rhs.messages) && body == rhs.body
            && names == rhs.names && is_equal(traced_rays, rhs.traced_rays) && is_equal(text_types, rhs.text_types)
            && attributes == rhs.attributes && is_equal(options, rhs.options) && float_guarantee == rhs.float_guarantee
            && float_raised_by == rhs.float_raised_by && written_words == rhs.written_words
            && zeroes_workgroup == rhs.zeroes_workgroup && unshadowed_bytes == rhs.unshadowed_bytes
            && workgroup_index == rhs.workgroup_index && is_equal(reserved, rhs.reserved)
            && is_equal(debug_sites, rhs.debug_sites);
    }
};
