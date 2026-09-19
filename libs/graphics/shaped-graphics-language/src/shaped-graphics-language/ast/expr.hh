#pragma once

#include <clean-core/container/variant.hh>
#include <shaped-graphics-language/ast/parts.hh>

/// Expressions are ONE family: a type is an ordinary expression standing in a type position.
/// The type positions are the members named `type`, `result` of a `function_type` and `return_type` of a function.
/// The builder looks no name up, so `vec3` is a `name` and `buffer[float]` an `index` wherever they stand.

enum class sgl::ast::literal_kind : sgl::u8
{
    number,
    /// The pieces and interpolations of a quoted literal stay reachable through the node's form.
    quoted,
    hash,
};

struct sgl::ast::literal
{
    literal_kind kind = literal_kind::number;

    constexpr bool operator==(literal const&) const = default;
};

struct sgl::ast::name
{
    source_span where;

    constexpr bool operator==(name const&) const = default;
};

/// `self`, which is a reserved name and no keyword: `self.x = 0` is an assignment like any other.
struct sgl::ast::self_ref
{
    constexpr bool operator==(self_ref const&) const = default;
};

struct sgl::ast::wildcard
{
    constexpr bool operator==(wildcard const&) const = default;
};

/// `.point`, `.0`
struct sgl::ast::leading_dot
{
    source_span name;

    constexpr bool operator==(leading_dot const&) const = default;
};

struct sgl::ast::member
{
    expr_id object = expr_id::none;
    source_span name;

    constexpr bool operator==(member const&) const = default;
};

/// A fused `a[…]`, which is a subscript or a type application; only resolution can tell.
struct sgl::ast::index
{
    expr_id object = expr_id::none;
    range_of<argument> arguments;

    constexpr bool operator==(index const&) const = default;
};

enum class sgl::ast::call_spelling : sgl::u8
{
    /// `f(x)`
    paren,
    /// `f x`
    juxtaposition,
    /// `a + b`
    infix,
    /// `-x`, `not x`
    prefix,
};

/// Every application of something to arguments, however it was written.
struct sgl::ast::call
{
    call_spelling spelling = call_spelling::paren;
    /// `none` for an operator call, whose callee is `op`.
    expr_id callee = expr_id::none;
    /// The operator token of an infix or prefix call, `none` otherwise.
    token_id op = token_id::none;
    range_of<argument> arguments;
    /// `and` and `or`: they type as ordinary functions and must not evaluate their second argument eagerly.
    bool is_short_circuit = false;

    constexpr bool operator==(call const&) const = default;
};

/// An unfused `(…)` with anything but exactly one plain element: `()`, `(x,)`, `(a, b)`.
/// `(x)` is `x` and leaves no node.
struct sgl::ast::tuple
{
    range_of<argument> elements;

    constexpr bool operator==(tuple const&) const = default;
};

struct sgl::ast::array
{
    range_of<argument> elements;

    constexpr bool operator==(array const&) const = default;
};

/// An unfused `{…}` that is not a `struct_type`; `{..defaults, roughness = 0.5}` is a record update.
struct sgl::ast::object
{
    range_of<argument> elements;

    constexpr bool operator==(object const&) const = default;
};

/// `0 <= i < n`: two or more comparisons, each operand evaluated once.
/// A single comparison is an infix `call`.
struct sgl::ast::comparison_chain
{
    /// One more than `operators`.
    range_of<expr_id> operands;
    range_of<token_id> operators;

    constexpr bool operator==(comparison_chain const&) const = default;
};

struct sgl::ast::cast
{
    expr_id value = expr_id::none;
    expr_id type = expr_id::none;

    constexpr bool operator==(cast const&) const = default;
};

/// `x in r`
struct sgl::ast::membership
{
    expr_id value = expr_id::none;
    expr_id container = expr_id::none;

    constexpr bool operator==(membership const&) const = default;
};

/// `x : t` in expression position.
struct sgl::ast::ascription
{
    expr_id value = expr_id::none;
    expr_id type = expr_id::none;

    constexpr bool operator==(ascription const&) const = default;
};

/// `a ..< b`, `a ..= b`; a range is a node of its own and never an infix `call`.
struct sgl::ast::range
{
    expr_id first = expr_id::none;
    expr_id last = expr_id::none;
    /// `..<`, `..=`, or the bare `..` the form parser has already reported.
    token_id op = token_id::none;

    constexpr bool operator==(range const&) const = default;
};

/// `x => body`, `_ => body`, `(a, b: int) => body`
struct sgl::ast::lambda
{
    range_of<field> parameters;
    sgl::ast::body body;

    constexpr bool operator==(lambda const&) const = default;
};

struct sgl::ast::case_expr
{
    expr_id value = expr_id::none;
    range_of<case_arm> arms;

    constexpr bool operator==(case_expr const&) const = default;
};

/// Yields through `break value`.
struct sgl::ast::loop_expr
{
    sgl::ast::body body;

    constexpr bool operator==(loop_expr const&) const = default;
};

/// The jumps are expressions, which is what lets `_ => return false` be a `case` arm.
struct sgl::ast::return_expr
{
    expr_id value = expr_id::none;

    constexpr bool operator==(return_expr const&) const = default;
};

struct sgl::ast::break_expr
{
    expr_id value = expr_id::none;

    constexpr bool operator==(break_expr const&) const = default;
};

struct sgl::ast::continue_expr
{
    constexpr bool operator==(continue_expr const&) const = default;
};

/// An unfused `{…}` whose elements are all `name: type`.
struct sgl::ast::struct_type
{
    range_of<field> fields;

    constexpr bool operator==(struct_type const&) const = default;
};

/// `(a, b) -> c`; a parameter is a `field`, unnamed unless written `name: type`.
struct sgl::ast::function_type
{
    range_of<field> parameters;
    expr_id result = expr_id::none;

    constexpr bool operator==(function_type const&) const = default;
};

/// A fused `{…}` in expression position, `f(x){…}`: reserved, and always reported as `unsupported-syntax`.
struct sgl::ast::with_bindings
{
    expr_id target = expr_id::none;
    range_of<argument> bindings;

    constexpr bool operator==(with_bindings const&) const = default;
};

/// What did not fit; `expr::form` keeps what was written, and a diagnostic says what was expected.
struct sgl::ast::invalid_expr
{
    constexpr bool operator==(invalid_expr const&) const = default;
};

struct sgl::ast::expr
{
    form_id form = form_id::none;
    /// Only an expression in a type position may carry attributes; anywhere else they are kept and reported.
    range_of<attribute> attributes;

    cc::variant<invalid_expr,
                literal,
                name,
                self_ref,
                wildcard,
                leading_dot,
                member,
                index,
                call,
                tuple,
                array,
                object,
                comparison_chain,
                cast,
                membership,
                ascription,
                range,
                lambda,
                case_expr,
                loop_expr,
                return_expr,
                break_expr,
                continue_expr,
                struct_type,
                function_type,
                with_bindings>
        node;

    bool operator==(expr const&) const = default;
};
