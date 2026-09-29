#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/builtins/ids.hh>
#include <shaped-graphics-language/check/features.hh>
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
    /// `&`, `|`, `^` and the shifts, which bind differently in every target: parenthesized wherever they are embedded.
    bitwise,
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
    /// Statements the target needs ahead of the one that holds the call, such as HLSL's `InterlockedAdd`, which gives
    /// the value before through an out parameter; `text` then reads what they left.
    cc::vector<cc::string> lines;
};

/// What a custom writer is given: the arguments already written, in order, and the registry for a type's spelling.
struct sgl::builtins::call_context
{
    language target = language::hlsl;
    cc::span<written const> arguments;
    registry const& builtins;
    /// The record's `spelling::data`, which lets one writer serve a family of records.
    u32 data = 0;
    /// A fresh name of the text, for a local that `written::lines` declares; none where no text is being written.
    cc::function_ref<cc::string(cc::string_view)> mint = {};
};

/// What a helper writer is given: the target, and each argument's type as the target spells it.
struct sgl::builtins::helper_context
{
    language target = language::hlsl;
    cc::span<cc::string const> argument_types;
    /// The record's `spelling::data`.
    u32 data = 0;
};

namespace sgl::builtins
{
using custom_writer = written (*)(call_context const&);
/// A function the text declares once, ahead of the entry point, for a call the target cannot write as one expression.
/// Empty for a target that needs none; two calls needing the same text get it once, so a helper may be an overload.
using helper_writer = cc::string (*)(helper_context const&);

/// Appends the result's scalars to `out`.
/// `in` holds the scalars of every argument, one argument behind the other.
/// The interpreter has checked their number and kind against the parameter types before it calls, and checks the result's after.
/// So one evaluator serves a whole family: `in.size() / 2` is the width of a componentwise binary operation.
using evaluator = void (*)(cc::span<check::scalar const> in, cc::vector<check::scalar>& out);

/// Why the call has no behaviour for these arguments, or empty when it has one; given what an evaluator is given.
/// A value no correct shader produces is left undefined on every target, and this is how the interpreter reports it.
using undefined_check = cc::string_view (*)(cc::span<check::scalar const> in);
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

/// The last argument of a call, which WGSL judges alone where it is constant, whatever stands beside it.
enum class sgl::builtins::judged_operand : sgl::u8
{
    none,
    /// A shift's count, which is 0 to 31 (CHK-270).
    shift_count,
    /// An integer divisor, no component of which is zero (CHK-311).
    divisor,
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
    helper_writer helper = nullptr;
    /// Whatever `custom` and `helper` read to tell the records they serve apart, such as which texture call it is.
    u32 data = 0;
    /// Every name `custom` and `helper` write in one language besides the arguments: a function called or declared, a
    /// scope reached into.
    /// A name of the program spelled alike would hide it, so the emitter spells none so (EMIT-15).
    /// A type the target predeclares need not stand here, since its reserved words hold it already.
    cc::span<cc::string_view const> hlsl_names;
    cc::span<cc::string_view const> wgsl_names;
    cc::span<cc::string_view const> msl_names;
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
    /// A bool crosses none in WGSL; an int crosses only flat, which the check pass holds it to (CHK-273).
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
    /// Empty for a builtin defined for every argument; otherwise the interpreter asks it before evaluating.
    undefined_check undefined_when = nullptr;
    /// Why WGSL refuses a call whose every argument is constant although EVAL gives it a value, such as an int sum
    /// outside the ints; empty where WGSL folds it (CHK-312).
    /// Given what an evaluator is given, and asked only where `undefined_when` found nothing.
    undefined_check unrepresentable_when_constant = nullptr;
    /// The argument the check pass judges alone where it is constant.
    judged_operand judged_last = judged_operand::none;
    spelling write;
    /// A texture method called without its sampler, which the texture's `@sampler` supplies at the call (CHK-279).
    /// The flattener calls this record instead, with the sampler inserted after the coordinate; `none` for every other.
    builtin_id with_default_sampler = builtin_id::none;
    /// Takes screen-space derivatives implicitly, as a sample that picks its own level does.
    /// So every pixel of the quad reaches the call together (CHK-282).
    bool uses_derivatives = false;
    /// Waits for every thread of the workgroup, so every one of them reaches the call or none does (CHK-282).
    bool is_barrier = false;
    /// Updates its first argument, an atomic, in one step (EVAL-93): the evaluator is given the atomic's value and then
    /// the other arguments, and gives what the atomic holds after; the call gives what it held before, or nothing.
    bool is_atomic = false;
    /// `nonuniform i`: its argument, marked as an index into a binding array that differs between invocations (CHK-300).
    bool is_nonuniform_mark = false;
    /// Takes one argument more than its signature names, of the type its first argument holds: a stream's `emit`,
    /// whose vertex is a struct of the program (CHK-303).
    bool takes_element = false;
    /// What a device needs to run a call of it: an entry point that reaches one needs it too, and declares it (CHK-322).
    check::feature_set features;
    /// Takes, past its signature, the position of its acceleration-structure argument among the acceleration members
    /// of the entry point's binding list, which flatten appends as an int: the emulated trace's root (CHK-325).
    bool takes_acceleration_index = false;
    /// Gives a value only a local may hold, which a target declares without an initializer: `RayQuery<…> q;`.
    /// The call itself writes nothing, and a local it initializes is never copied.
    bool declares_only = false;

    /// Read back from the signature by `finalize`.
    cc::string name;
    /// Each parameter's type as the signature spells it: a builtin type's name, or a resource pattern such as
    /// `out image_2d[float4]`, which the check pass matches by the same spelling.
    cc::vector<cc::string> parameters;
    /// Parallel to `parameters`: a named-only parameter's name, empty for a positional one.
    /// Two records whose types agree are still two overloads when these differ: `.level: float` and `.bias: float`.
    cc::vector<cc::string> named_only;
    /// `none` for a function that gives nothing, which only one with an effect can be.
    builtin_type_id result = builtin_type_id::none;

    /// The name a `call` has in `l`.
    [[nodiscard]] cc::string_view called_in(language l) const;
    /// `spelling::hlsl_names` and its siblings, by language.
    [[nodiscard]] cc::span<cc::string_view const> names_in(language l) const;
    /// True for the name a `call` has in `l`, and for every name of `names_in(l)`: a name of the program must not be it.
    [[nodiscard]] bool writes_name(language l, cc::string_view name) const;
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
    /// Every overload of a name, in registration order; filled by `finalize`, which is when a record learns its name.
    cc::map<cc::string, cc::vector<builtin_id>> functions_by_name;
    /// The overloads whose name, parameters and named-only names hash alike (`signature_hash`); filled by `finalize` too.
    cc::map<u64, cc::vector<builtin_id>> functions_by_signature;

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
    /// The overload of `name` that takes exactly `parameters`, named-only as `named_only` says; `none` when there is none.
    /// Both lookups read `functions_by_name`, so the registry must be finalized.
    [[nodiscard]] builtin_id find_function(cc::string_view name,
                                           cc::span<cc::string_view const> parameters,
                                           cc::span<cc::string_view const> named_only) const;
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
/// `op` must be one of `+ - * / % < <= > >= == !=`.
[[nodiscard]] spelling infix(cc::string_view op);

/// The names of the types the compiler itself needs: a literal's type, a condition's, a clip-space position's.
constexpr cc::string_view k_float = "float";
constexpr cc::string_view k_int = "int";
constexpr cc::string_view k_uint = "uint";
constexpr cc::string_view k_bool = "bool";
constexpr cc::string_view k_hpos4 = "hpos4";

/// Every builtin SGL has, registered in a fixed order and finalized.
[[nodiscard]] registry make_registry();

/// `make_registry()`, built on first use and kept for the life of the process.
/// The one cached value in the library: building it parses the whole prelude, and every compile needs it.
/// It is immutable, so it is state nobody can observe changing.
[[nodiscard]] registry const& default_registry();
} // namespace sgl::builtins
