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
    /// `void`, what a function without a return type returns.
    void_,
    /// A declared `struct`, builtin or not; two declarations are two types, whatever their fields.
    structure,
    /// A declared `enum`: a closed set of named `int` values that converts to nothing (CHK-142, CHK-150).
    enumeration,
    /// A `buffer[T]`: an array of `element` a shader indexes, and `mut` where it may be written (the spec's bindings file).
    /// It is a resource rather than a value: it stands in a binding, and nothing loads or copies one.
    buffer,
    /// A sampled texture of one `shape`, whose samples are `element`, or a depth texture where `is_depth`.
    texture,
    /// A storage texture of one `shape` and `format`, which the shader reads, writes or both by its `access`.
    image,
    /// A sampler, filtering or `is_comparison`; a resource like a texture, never a value.
    sampler,
    // Tuples, function types and anonymous struct types come later, each as a kind that is deduplicated by structure.
};

namespace sgl::check
{
/// True for a kind that stands in a binding and is never a value: a buffer, a texture, an image or a sampler.
[[nodiscard]] constexpr bool is_resource(type_kind k)
{
    return k == type_kind::buffer || k == type_kind::texture || k == type_kind::image || k == type_kind::sampler;
}
} // namespace sgl::check

namespace sgl::check
{
/// The bit of `s` in a set of stages, as `function_info::stages` holds one.
[[nodiscard]] constexpr u8 stage_bit(stage s)
{
    return u8(1u << u8(s));
}
inline constexpr u8 k_every_stage = 0xFF;
} // namespace sgl::check

/// What a shader may do with an image: unmarked, `mut` and `out` (the spec's bindings file, "Access").
enum class sgl::check::image_access : sgl::u8
{
    read,
    read_write,
    write,
};

/// A static sampler's settings, each a field of `sg::sampler`; an enum setting is a position in its table of names.
struct sgl::check::sampler_state
{
    /// Positions in `k_sampler_filters`: nearest 0, linear 1.
    u8 min_filter = 1;
    u8 mag_filter = 1;
    u8 mip_filter = 1;
    /// Positions in `k_sampler_addresses`: repeat 0.
    u8 address_u = 0;
    u8 address_v = 0;
    u8 address_w = 0;
    /// A position in `k_compare_ops`, or -1 for a sampler that compares nothing.
    i32 compare = -1;
    i32 max_anisotropy = 1;
    f32 min_lod = 0.0f;
    /// Absent is unclamped.
    f32 max_lod = 3.4028235e38f;
    f32 mip_lod_bias = 0.0f;

    constexpr bool operator==(sampler_state const&) const = default;
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
    /// The shape of a `texture` or an `image`.
    texture_shape shape = {};
    /// A `texture` that holds depth, which takes no `element`.
    bool is_depth = false;
    /// A position in `k_storage_formats` for an `image`; -1 for every other kind.
    i32 format = -1;
    image_access access = image_access::read;
    /// A `sampler` that compares.
    bool is_comparison = false;
    /// How a resource type is written, `out image2d[.rgba8_unorm]`; empty for a declared type, which its symbol names.
    cc::string spelled;

    bool operator==(type_info const&) const = default;
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
    /// Carries `@unfilterable`: a texture whose samples are never filtered.
    bool is_unfilterable = false;
    /// Carries `@non_filtering`: a sampler that never filters.
    bool is_non_filtering = false;
    /// A `sampler name:` block of a binding, as a position in `checked_module::samplers`; -1 for any other member.
    i32 static_sampler = -1;

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
    /// A `pipeline` declaration; an unnamed one is named `pipeline`.
    pipeline,
    /// A file-scope `const`, whose value is known before anything runs.
    constant,
    /// A `test`, which has no name and which no lookup finds; `info` is its synthesized signature.
    test,
    /// A named declaration this phase has no meaning for yet: `type`, `sampler`.
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
    /// A position in `checked_module::functions`, `bindings`, `pipelines` or `constants`, by `kind`; -1 before it is compiled.
    i32 info = -1;
    /// False under `@shadowable(false)`: a declaration or a local of its name is then an error rather than hiding it.
    bool is_shadowable = true;

    bool operator==(symbol const&) const = default;
};

enum class sgl::check::constant_kind : sgl::u8
{
    integer,
    real,
    /// A case of an enum; for `bool`, whose cases are its two values, the case is the value.
    enum_case,
};

/// The value of a `const`, which is known before anything runs.
struct sgl::check::constant_info
{
    symbol_id symbol = symbol_id::none;
    type_id type = type_id::none;
    constant_kind kind = constant_kind::integer;
    i32 integer = 0;
    f64 real = 0;
    /// A position in the `cases` of `type`, for an `enum_case`.
    i32 case_index = -1;

    bool operator==(constant_info const&) const = default;
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
    /// The stages an entry point may be of to reach it, one bit per `stage` (`stage_bit`); every stage without `@stages`.
    u8 stages = k_every_stage;

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

/// What a pipeline setting's value is.
enum class sgl::check::setting_kind : sgl::u8
{
    boolean,
    integer,
    real,
    /// A case of the enum the setting's field has, by name: sg's enum of the same name is what it becomes.
    enum_case,
    /// `.host`: the host states it when it acquires the pipeline.
    host,
    /// `blend = .none`: the optional part is switched off.
    none,
};

/// Where a pipeline setting was written, which is the order the settings apply in.
enum class sgl::check::setting_source : sgl::u8
{
    /// An attribute of the vertex input or of the `@pixel struct`, or of one of its members.
    edge_struct,
    /// An attribute of an entry point.
    stage,
    /// A line of the `pipeline` declaration.
    declaration,
};

/// One field of a pipeline's description, written once.
/// A whole struct written at once is one of these per field it has, so every setting is a leaf.
struct sgl::check::pipeline_setting
{
    /// From the description down, with a target's member name where sg has an index: `color_targets.albedo.format`.
    cc::string path;
    setting_kind kind = setting_kind::boolean;
    /// 0 or 1 for a `boolean`, the value of an `integer`.
    i64 integer = 0;
    f64 real = 0;
    /// The case name of an `enum_case`, and the enum it is a case of, which is sg's enum of the same name.
    cc::string enum_case;
    cc::string enum_name;
    setting_source source = setting_source::declaration;
    /// Where it was written, in that file.
    i32 file = 0;
    source_span where;

    bool operator==(pipeline_setting const&) const = default;
};

enum class sgl::check::pipeline_kind : sgl::u8
{
    raster,
    compute,
    raytracing,
};

/// A `pipeline` declaration that checked: its stages, its layout, and its configuration.
struct sgl::check::pipeline_info
{
    symbol_id symbol = symbol_id::none;
    pipeline_kind kind = pipeline_kind::raster;
    symbol_id vertex = symbol_id::none;
    /// `none` for a pipeline without a pixel stage, which writes depth alone.
    symbol_id pixel = symbol_id::none;
    /// The binding layout: the longest binding list of its stages with `@inline` left out; a range of `binding_lists`.
    ast::range_of<symbol_id> layout;
    /// The one `@inline` binding its stages list, or `none`.
    symbol_id inline_constants = symbol_id::none;
    /// The vertex stage's parameter, and the pixel stage's result; `none` without a pixel stage.
    type_id vertex_input = type_id::none;
    type_id target_set = type_id::none;
    /// In the order they apply, each over the ones before it and all over sg's defaults.
    ast::range_of<pipeline_setting> settings;

    constexpr bool operator==(pipeline_info const&) const = default;
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
