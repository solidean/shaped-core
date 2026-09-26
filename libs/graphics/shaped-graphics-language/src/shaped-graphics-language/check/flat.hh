#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/container/vector.hh>
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
    /// Every name handed out or reserved so far.
    cc::vector<cc::string> taken;

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

    bool operator==(flat_local const&) const = default;
};

/// The name of a block or a loop, which a `leave` or a `continue` refers to.
/// No target writes it: it is for a dump, and the legalizer names a flag after it.
struct sgl::check::flat_label
{
    cc::string name;

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

/// Of the prelude's type `int`, or of `uint` where an integer literal converted to it (CHK-253).
struct sgl::check::flat_int_literal
{
    i32 value = 0;
    bool is_unsigned = false;

    constexpr bool operator==(flat_int_literal const&) const = default;
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

    constexpr bool operator==(flat_binding_member const&) const = default;
};

struct sgl::check::flat_member
{
    flat_expr_id object = flat_expr_id::none;
    /// A position in the `members` of the object's struct type.
    i32 member = -1;

    constexpr bool operator==(flat_member const&) const = default;
};

/// `values[i]` on a `buffer`: the element a shader loads, and the place it stores to.
/// `buffer` is what names the resource, which today is always a `flat_binding_member`.
/// As a place, `index` is evaluated before the value that is stored.
struct sgl::check::flat_buffer_element
{
    flat_expr_id buffer = flat_expr_id::none;
    flat_expr_id index = flat_expr_id::none;

    constexpr bool operator==(flat_buffer_element const&) const = default;
};

/// A value of the node's struct type from one value per field, in field order.
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

struct sgl::check::flat_expr
{
    type_id type = type_id::none;
    origin from;
    /// A range of `flat_entry_point::call_sites`; empty for a node of the entry point's own body.
    ast::range_of<call_site> inlined_through;

    cc::variant<flat_invalid,
                flat_literal,
                flat_int_literal,
                flat_bool_literal,
                flat_enum_value,
                flat_local_ref,
                flat_binding_member,
                flat_member,
                flat_buffer_element,
                flat_construct,
                flat_call,
                flat_not,
                flat_and,
                flat_or,
                flat_block>
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
struct sgl::check::flat_assign
{
    flat_expr_id place = flat_expr_id::none;
    flat_expr_id value = flat_expr_id::none;

    constexpr bool operator==(flat_assign const&) const = default;
};

/// Records `value`, which is the one effect a program has besides its result.
/// No emitter writes it yet.
struct sgl::check::flat_print
{
    flat_expr_id value = flat_expr_id::none;

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
                flat_once,
                flat_break,
                flat_case,
                flat_switch,
                flat_return,
                flat_check>
        node;

    bool operator==(flat_stmt const&) const = default;
};

/// One entry point as one flat function.
/// A type, a symbol and a binding are ids into the `checked_module` this value stands in.
struct sgl::check::flat_entry_point
{
    stage entry_stage = stage::none;
    /// The name as written: the host asks for it, so it is never minted.
    cc::string name;
    symbol_id function = symbol_id::none;
    /// The one parameter, which is `locals[0]`.
    type_id input = type_id::none;
    type_id result = type_id::none;
    /// The bindings of the function's `{...}` list in the order written, which is what decides the pipeline layout.
    cc::vector<symbol_id> bindings;
    /// The grid a `compute` entry point is dispatched in; `{1, 1, 1}` for every other stage.
    i32 workgroup[3] = {1, 1, 1};
    /// The parameter carries `@thread_id` itself rather than being a struct that holds one.
    bool takes_thread_id = false;

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
            && takes_thread_id == rhs.takes_thread_id && is_equal(locals, rhs.locals) && is_equal(labels, rhs.labels)
            && root == rhs.root && is_equal(exprs, rhs.exprs) && is_equal(stmts, rhs.stmts)
            && is_equal(expr_lists, rhs.expr_lists) && is_equal(stmt_lists, rhs.stmt_lists) && is_equal(arms, rhs.arms)
            && is_equal(call_sites, rhs.call_sites) && is_equal(check_sites, rhs.check_sites)
            && is_equal(check_nodes, rhs.check_nodes) && body == rhs.body && names == rhs.names;
    }
};
