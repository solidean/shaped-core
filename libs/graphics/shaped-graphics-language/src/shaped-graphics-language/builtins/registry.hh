#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/builtins/ids.hh>
#include <shaped-graphics-language/interpret/scalar.hh>

/// Everything the compiler knows about a builtin, in ONE record per builtin.
///
/// C++ is the source of truth: `prelude/builtins.sgl` is `registry::prelude_text()`, committed and checked.
/// A record's signature is SGL source text, and the text the normal parser reads is all there is to it.
/// What `finalize` fills in — a name, the parameter types — is read back from that parse, never stated a second time.
/// libs/graphics/shaped-graphics-language/docs/adding-a-builtin.md is the walk-through.

/// The three text formats; the two HLSL targets spell every builtin alike.
enum class sgl::builtins::language : sgl::u8
{
    hlsl,
    wgsl,
    msl,
};

/// How tightly a written expression holds together, loosest first; the same ladder in every target so far.
enum class sgl::builtins::precedence : sgl::u8
{
    logical_or,
    logical_and,
    /// `<` and `==`, which no target chains.
    comparison,
    additive,
    multiplicative,
    /// A negative literal, a prefix operator.
    unary,
    /// A name, a call, a construction, a member access: whatever needs no parentheses anywhere.
    primary,
};

/// A piece of target text and how tightly it binds, so that whoever embeds it knows whether it needs parentheses.
struct sgl::builtins::written
{
    cc::string text;
    precedence binds = precedence::primary;
};

/// What a custom writer is given: the arguments already written, in order, and the registry for a type's spelling.
struct sgl::builtins::call_context
{
    language target = language::hlsl;
    cc::span<written const> arguments;
    registry const& builtins;
};

namespace sgl::builtins
{
using custom_writer = written (*)(call_context const&);

/// Appends the result's scalars to `out`.
/// `in` holds the scalars of every argument, one argument behind the other.
/// The interpreter has checked their number and kind against the parameter types before it calls, and checks the result's after.
/// So one evaluator serves a whole family: `in.size() / 2` is the width of a componentwise binary operation.
using evaluator = void (*)(cc::span<check::scalar const> in, cc::vector<check::scalar>& out);
} // namespace sgl::builtins

enum class sgl::builtins::spelling_kind : sgl::u8
{
    /// `name(a, b)`.
    call,
    /// `a op b`.
    infix,
    /// `op a`.
    prefix,
    /// Whatever `spelling::custom` writes.
    custom,
};

/// How every target writes a call of one builtin function.
struct sgl::builtins::spelling
{
    spelling_kind kind = spelling_kind::call;
    /// The function name of a `call`, where empty means the SGL name; the operator of an `infix` or a `prefix`.
    cc::string text;
    /// A `call` under another name in one language: `.hlsl = "lerp"`.
    cc::string hlsl;
    cc::string wgsl;
    cc::string msl;
    /// How the result of an `infix` binds.
    precedence binds = precedence::primary;
    custom_writer custom = nullptr;
};

/// Where a value of a builtin type lands in a constant block; a size of 0 means it has no place in one.
struct sgl::builtins::block_layout
{
    i32 size = 0;
    i32 alignment = 0;
};

struct sgl::builtins::type_record
{
    /// The declaration as SGL source, without `@builtin`: `struct mat4`, or `struct float3:` and its field lines.
    cc::string declaration;
    /// Zero or more whole `///` lines, without the line break of the last one.
    cc::string doc;

    cc::string hlsl;
    cc::string wgsl;
    cc::string msl;

    /// HLSL packs by rows of 16: a value starts a fresh row when its alignment is 16, or when it does not fit the rest of this one.
    block_layout hlsl_layout;
    block_layout wgsl_layout;
    block_layout msl_layout;

    /// A value is `leaf_count` scalars of `leaf_kind`; a `mat4` is 16, column by column.
    check::value_kind leaf_kind = check::value_kind::none;
    i32 leaf_count = 0;

    /// May be a member of a struct that crosses a stage edge.
    /// A bool crosses none in WGSL, and an int would need a flat interpolation nothing states yet.
    bool crosses_edges = false;

    /// Read back from the declaration by `finalize`.
    cc::string name;

    [[nodiscard]] cc::string_view spelled_in(language l) const;
};

struct sgl::builtins::function_record
{
    /// The signature as SGL source, without `@builtin`: `@pure fun mix(a: float3, b: float3, t: float) -> float3`.
    /// `@pure` and `@operator("…")` stand in this text and nowhere else.
    cc::string signature;
    /// Zero or more whole `///` lines, without the line break of the last one.
    cc::string doc;
    evaluator evaluate = nullptr;
    spelling write;

    /// Read back from the signature by `finalize`.
    cc::string name;
    /// Each parameter's type as the signature spells it: a builtin type's name, or a resource pattern such as
    /// `out image2d[float4]`, which the check pass matches by the same spelling.
    cc::vector<cc::string> parameters;
    /// `none` for a function that gives nothing, which only one with an effect can be.
    builtin_type_id result = builtin_type_id::none;

    /// The name a `call` has in `l`.
    [[nodiscard]] cc::string_view called_in(language l) const;
};

/// One piece of the generated file, in the order it was registered.
struct sgl::builtins::registry_item
{
    enum class kind_t : u8
    {
        comment,
        type,
        function,
    };
    kind_t kind = kind_t::comment;
    /// A position in `types` or `functions`; -1 for a comment.
    i32 index = -1;
    cc::string comment;
};

/// A value: built by `make_registry`, and nothing registers itself into it.
struct sgl::builtins::registry
{
    cc::vector<type_record> types;
    cc::vector<function_record> functions;
    cc::vector<registry_item> items;

    builtin_type_id add(type_record record);
    builtin_id add(function_record record);
    /// A `//` comment block of the generated file, behind an empty line; `text` is whole lines without the last line break.
    void add_comment(cc::string_view text);

    /// Parses `prelude_text()` with the normal parser and fills what every record reads back from it.
    /// Every record must parse to exactly one declaration without a diagnostic, and every type a signature names must be registered.
    void finalize();

    [[nodiscard]] type_record const& at(builtin_type_id id) const { return types[index_of(id)]; }
    [[nodiscard]] function_record const& at(builtin_id id) const { return functions[index_of(id)]; }
    [[nodiscard]] bool is_known(builtin_type_id id) const { return is_valid(id) && index_of(id) < types.size(); }
    [[nodiscard]] bool is_known(builtin_id id) const { return is_valid(id) && index_of(id) < functions.size(); }

    /// `none` for a name no type was registered under.
    [[nodiscard]] builtin_type_id find_type(cc::string_view name) const;
    /// The overload of `name` that takes exactly `parameters`; `none` when there is none.
    [[nodiscard]] builtin_id find_function(cc::string_view name, cc::span<cc::string_view const> parameters) const;
    [[nodiscard]] bool has_function_named(cc::string_view name) const;

    /// The whole of `prelude/builtins.sgl`, byte for byte, in registration order.
    [[nodiscard]] cc::string prelude_text() const;
};

namespace sgl::builtins
{
/// `w.text`, in parentheses when it binds looser than `needed`.
[[nodiscard]] cc::string wrapped(written w, precedence needed);

/// `lhs op rhs`, left-associative: an equal level on the right keeps its parentheses, since `a + (b + c)` is not `a + b + c` in floats.
[[nodiscard]] written write_infix(cc::string_view op, precedence own, written lhs, written rhs);

/// `a op b` at the level `op` has in every target.
/// `op` must be one of `+ - * / < <= > >= == !=`.
[[nodiscard]] spelling infix(cc::string_view op);

/// The names of the types the compiler itself needs: a literal's type, a condition's, a clip-space position's.
constexpr cc::string_view k_float = "float";
constexpr cc::string_view k_int = "int";
constexpr cc::string_view k_bool = "bool";
constexpr cc::string_view k_hpos4 = "hpos4";

/// Every builtin SGL has, registered in a fixed order and finalized.
[[nodiscard]] registry make_registry();

/// `make_registry()`, built on first use and kept for the life of the process.
/// The one cached value in the library: building it parses the whole prelude, and every compile needs it.
/// It is immutable, so it is state nobody can observe changing.
[[nodiscard]] registry const& default_registry();
} // namespace sgl::builtins
