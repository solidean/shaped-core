#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/builtins/ids.hh>
#include <shaped-graphics-language/check/ids.hh>

/// What the check pass knows about the declarations of a module: its types, its symbols and the per-file side tables.
/// Every name is kept as text, so a reader of these needs no `parsed_file`.

enum class sgl::check::type_kind : sgl::u8
{
    /// The type of whatever did not check.
    /// It equals every type for the purpose of reporting, so one error never causes a second diagnostic.
    error,
    /// What a function without a return type returns: no value, so nothing can hold it.
    nothing,
    /// A declared `struct`, builtin or not; two declarations are two types, whatever their fields.
    structure,
    /// A declared `enum`: a closed set of named `int` values that converts to nothing (CHK-142, CHK-150).
    enumeration,
    /// A `buffer[T]`: an array of `element` a shader indexes, and `mut` where it may be written (the spec's bindings file).
    /// It is a resource rather than a value: it stands in a binding, and nothing loads or copies one.
    buffer,
    // Tuples, function types and anonymous struct types come later, each as a kind that is deduplicated by structure.
};

/// A pipeline stage, on an entry point and on the struct that describes its edge of the pipeline.
enum class sgl::check::stage : sgl::u8
{
    none,
    vertex,
    pixel,
    /// A `@compute(x, y, z)` fun: it is dispatched over a grid, returns nothing, and reads which thread it is.
    compute,
};

/// One canonical type: equal types have equal ids, so type equality is id equality.
struct sgl::check::type_info
{
    type_kind kind = type_kind::error;
    /// The declaring `struct`; `none` for the error type.
    symbol_id symbol = symbol_id::none;
    /// The fields in declaration order, which is the order of the synthesized constructor's parameters.
    ast::range_of<member_info> members;
    /// The cases of an `enumeration`, in declaration order; empty for every other kind.
    ast::range_of<enum_case_info> cases;
    /// Declared without a block: no member can be named and no constructor exists.
    bool is_opaque = false;
    /// `@vertex struct` is a vertex input and `@pixel struct` a set of render targets.
    stage edge = stage::none;
    /// The element of a `buffer`; `none` for every other kind.
    type_id element = type_id::none;
    /// Whether a `buffer` may be written: `mut buffer[T]` against `buffer[T]`.
    bool is_mut = false;

    constexpr bool operator==(type_info const&) const = default;
};

/// A field of a struct or a member of a binding.
struct sgl::check::member_info
{
    cc::string name;
    /// The error type when the member's type did not resolve.
    type_id type = type_id::none;
    /// In the file of the owning symbol.
    ast::field_id field = ast::field_id::none;
    /// Carries `@position`.
    bool is_position = false;
    /// Carries `@thread_id`: which thread of the dispatch is running, as an `int3`.
    bool is_thread_id = false;
    /// Carries `@per_instance`: in a vertex input, the member steps once per instance.
    bool is_per_instance = false;
    /// The name `@stream(name)` gives; empty without one.
    cc::string stream;

    bool operator==(member_info const&) const = default;
};

/// One case of an `enum`: a name and the `int` value a target compares.
/// Two cases may hold one value, so a value does not name a case (CHK-145).
struct sgl::check::enum_case_info
{
    cc::string name;
    i32 value = 0;
    /// In the file of the owning symbol.
    ast::decl_id declaration = ast::decl_id::none;

    bool operator==(enum_case_info const&) const = default;
};

enum class sgl::check::symbol_kind : sgl::u8
{
    structure,
    enumeration,
    function,
    binding,
    /// A named declaration this phase has no meaning for yet: `const`, `type`, `sampler`.
    /// It is always `failed`, and it exists so its name resolves to the error type and not to `unknown-name`.
    unsupported,
};

/// Where the demand-driven pass is with one symbol.
enum class sgl::check::symbol_state : sgl::u8
{
    untouched,
    /// Somebody is compiling it right now.
    /// Reaching such a symbol again is a dependency cycle today; with an asynchronous pass it is what one awaits.
    in_compilation,
    checked,
    /// Nothing about it can be relied on; whatever needs it gets the error type and reports nothing further.
    failed,
};

/// One module-level declaration, named by the file it stands in and its declaration there.
struct sgl::check::symbol
{
    i32 file = 0;
    ast::decl_id declaration = ast::decl_id::none;
    symbol_kind kind = symbol_kind::unsupported;
    symbol_state state = symbol_state::untouched;
    cc::string name;
    /// The registry record a `@builtin fun` stands for; `none` for a struct and for a declaration of the program's own.
    builtin_id intrinsic = builtin_id::none;
    /// The registry record a `@builtin struct` stands for; `none` for everything else.
    builtin_type_id intrinsic_type = builtin_type_id::none;
    /// The operator of an `@operator` function, which lookup finds through this spelling and never through `name`.
    cc::string operator_spelling;
    /// A struct's type.
    type_id type = type_id::none;
    /// A position in `checked_module::functions` or `checked_module::bindings`, by `kind`; -1 before it is compiled.
    i32 info = -1;

    bool operator==(symbol const&) const = default;
};

struct sgl::check::parameter
{
    cc::string name;
    type_id type = type_id::none;
    ast::field_id field = ast::field_id::none;
    /// Carries `@thread_id`, which a compute entry point may write instead of a struct.
    bool is_thread_id = false;

    bool operator==(parameter const&) const = default;
};

/// The signature of a function, which is all a caller needs.
struct sgl::check::function_info
{
    symbol_id symbol = symbol_id::none;
    ast::range_of<parameter> parameters;
    type_id result = type_id::none;
    /// The bindings of the `{...}` list, in the order written; a range of `checked_module::binding_lists`.
    ast::range_of<symbol_id> bindings;
    /// `@vertex`, `@pixel` or `@compute` makes the function an entry point.
    stage entry_stage = stage::none;
    /// The grid a `@compute` entry point is dispatched in, from `@compute(x, y, z)`; 1 for an axis nobody wrote.
    i32 workgroup[3] = {1, 1, 1};
    /// Carries `@pure`: a call of it has no effect, so nobody can tell whether or when it ran.
    /// A `@builtin` without it is assumed to have one.
    bool is_pure = false;

    constexpr bool operator==(function_info const&) const = default;
};

struct sgl::check::binding_info
{
    symbol_id symbol = symbol_id::none;
    /// `@inline`: the members ride as inline constants, which an emitter must know.
    bool is_inline = false;
    ast::range_of<member_info> members;

    constexpr bool operator==(binding_info const&) const = default;
};

enum class sgl::check::target_kind : sgl::u8
{
    none,
    /// `index` is the `stmt_id` of the `let` or of the `for` that declares it, in the expression's own file.
    local,
    /// `index` is the `field_id` of the parameter, in the expression's own file.
    parameter,
    /// A name that stands for a struct or a binding.
    symbol,
    /// On a call and on its callee name: the function overload resolution chose.
    overload,
    /// On a call and on its callee name: the synthesized constructor of the struct `symbol`.
    constructor,
    /// On a `member`: field `index` of the struct `symbol`.
    field,
    /// On a `member`: member `index` of the binding `symbol`.
    binding_member,
    /// On a `member` or a `leading_dot`: case `index` of the enum `symbol`.
    enum_case,
};

/// What an expression refers to, for an editor: go to definition, hover, rename.
struct sgl::check::target
{
    target_kind kind = target_kind::none;
    symbol_id symbol = symbol_id::none;
    i32 index = -1;

    constexpr bool operator==(target const&) const = default;
};

/// The side tables over one file's untouched AST, both parallel to `file_ast::exprs`.
struct sgl::check::file_tables
{
    /// `none` for an expression nothing checked; an expression in a type position has the type it names.
    cc::vector<type_id> type_of;
    cc::vector<target> target_of;

    [[nodiscard]] type_id type_at(ast::expr_id id) const { return type_of[ast::index_of(id)]; }
    [[nodiscard]] target const& target_at(ast::expr_id id) const { return target_of[ast::index_of(id)]; }

    [[nodiscard]] bool operator==(file_tables const& rhs) const
    {
        return ast::impl::is_equal(type_of, rhs.type_of) && ast::impl::is_equal(target_of, rhs.target_of);
    }
};
