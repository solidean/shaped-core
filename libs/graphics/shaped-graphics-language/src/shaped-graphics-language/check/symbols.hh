#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/builtins/ids.hh>
#include <shaped-graphics-language/check/features.hh>
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
    /// `T[N]`: `count` values of `element`, a value like a struct (CHK-285); `T[a, b]` is an array of arrays.
    array,
    /// `atomic[uint]` or `atomic[int]`: memory every invocation updates in one indivisible step (CHK-296).
    /// It stands in a `mut buffer` or in workgroup memory, and like a resource it is never a value: builtins take it.
    atomic,
    /// `point_stream[T]`, `line_stream[T]` or `triangle_stream[T]`: what a geometry stage appends vertices `T` to,
    /// `count` of them a primitive; only a geometry entry point's parameter is one (CHK-302).
    stream,
    /// A `buffer[T]`: an array of `element` a shader indexes, and `mut` where it may be written (the spec's bindings file).
    /// It is a resource rather than a value: it stands in a binding, and nothing loads or copies one.
    buffer,
    /// A sampled texture of one `shape`, whose samples are `element`, or a depth texture where `is_depth`.
    texture,
    /// A storage texture of one `shape` and `format`, which the shader reads, writes or both by its `access`.
    image,
    /// A sampler, filtering or `is_comparison`; a resource like a texture, never a value.
    sampler,
    /// `acceleration_structure[.triangles]`: what a trace runs against, a TLAS the host binds (CHK-320).
    /// `format` is its geometry, a position in `k_geometry_kinds`; a resource, never a value.
    acceleration_structure,
    /// `(A, B) -> R`: a function a parameter takes, whose `members` are the parameter types and `element` the result.
    /// Only a parameter holds one, and a call through it is inlined where the function was handed over (CHK-317).
    function,
    /// `A` of `fun f[A](…)` or of a generic prelude struct: a type nothing is known of, which a value of is handed on,
    /// stored and returned, and nothing else (CHK-338).
    /// A call deduces what it stands for, and inlining writes that in its place.
    type_parameter,
    // Tuples and anonymous struct types come later, each as a kind that is deduplicated by structure.
};

namespace sgl::check
{
/// True for a kind that stands in a binding and is never a value: a buffer, a texture, an image or a sampler.
[[nodiscard]] constexpr bool is_resource(type_kind k)
{
    return k == type_kind::buffer || k == type_kind::texture || k == type_kind::image || k == type_kind::sampler
        || k == type_kind::acceleration_structure;
}
} // namespace sgl::check

namespace sgl::check
{
/// The bit of `s` in a set of stages, as `function_info::stages` holds one.
[[nodiscard]] constexpr u16 stage_bit(stage s)
{
    return u16(1u << u8(s));
}
inline constexpr u16 k_every_stage = 0xFFFF;
} // namespace sgl::check

/// What a shader may do with an image: unmarked, `mut` and `out` (the spec's bindings file, "Access").
enum class sgl::check::access_mode : sgl::u8
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
    /// A `@geometry(max_vertices = N)` fun: it takes one primitive's vertices and appends vertices to a stream (CHK-301).
    geometry,
    /// A `@tessellation_control` fun: it takes a patch and returns its tessellation factors (CHK-304).
    tessellation_control,
    /// A `@tessellation_evaluation` fun: it takes a patch, its factors and a point of the domain, and returns a vertex (CHK-306).
    tessellation_evaluation,
    /// The ray-tracing stages (CHK-326): where a dispatch of rays starts, one invocation per launch index.
    raygen,
    /// What a ray that hit nothing runs, with its ray type's payload.
    miss,
    /// What the nearest accepted hit runs, once per trace.
    closest_hit,
    /// What decides a candidate the traversal could not decide alone.
    any_hit,
    /// What finds the hits in a procedural primitive's box.
    intersection,
    /// A function another ray-tracing stage calls through a table.
    callable,
};

namespace sgl::check
{
/// A stage as its attribute spells it: `vertex`, `tessellation_control`.
[[nodiscard]] constexpr cc::string_view stage_name(stage s)
{
    switch (s)
    {
    case stage::vertex:
        return "vertex";
    case stage::pixel:
        return "pixel";
    case stage::compute:
        return "compute";
    case stage::geometry:
        return "geometry";
    case stage::tessellation_control:
        return "tessellation_control";
    case stage::tessellation_evaluation:
        return "tessellation_evaluation";
    case stage::raygen:
        return "raygen";
    case stage::miss:
        return "miss";
    case stage::closest_hit:
        return "closest_hit";
    case stage::any_hit:
        return "any_hit";
    case stage::intersection:
        return "intersection";
    case stage::callable:
        return "callable";
    case stage::none:
        break;
    }
    return "no";
}
} // namespace sgl::check

/// Which tessellation factors a member of a factors struct holds (CHK-305).
enum class sgl::check::tessellation_factor : sgl::u8
{
    none,
    edge,
    inside,
};

/// How the tessellator spaces what a factor asks for (CHK-304); vulkan has no power-of-two spacing.
enum class sgl::check::tessellation_partitioning : sgl::u8
{
    integer,
    fractional_even,
    fractional_odd,
};

/// A value the GPU hands an invocation, which an entry point takes as a parameter marked with its attribute (CHK-271).
enum class sgl::check::stage_input : sgl::u8
{
    none,
    vertex_index,
    instance_index,
    is_front_facing,
    sample_index,
    sample_mask,
    primitive_id,
    thread_id,
    local_thread_id,
    local_thread_index,
    workgroup_id,
    /// Where in the tessellated domain the evaluation stage runs: barycentric for triangles, `(u, v)` otherwise.
    domain_location,
    /// A ray-tracing stage's launch index, and the size of the launch (CHK-327).
    launch_id,
    launch_size,
};

/// What the checker knows of one stage input: the attribute, the stage that has it, its type and the feature it needs.
struct sgl::check::stage_input_info
{
    stage_input input = stage_input::none;
    /// The attribute without its `@`, which is also the input's name in a diagnostic.
    cc::string_view name;
    stage in_stage = stage::none;
    /// The other stages that have it, each without a feature: a `stage_bit` mask.
    u16 also_in = 0;
    /// The name of its builtin type.
    cc::string_view type;
    /// -1 for an input every device has; otherwise a `feature` (check/features.hh).
    i32 feature = -1;
};

namespace sgl::check
{
/// Every stage input, `none` excepted, in the order of the enum.
[[nodiscard]] cc::span<stage_input_info const> stage_inputs();
/// The one of `input`; `input` must not be `none`.
[[nodiscard]] stage_input_info const& info_of(stage_input input);
} // namespace sgl::check

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
    /// `@no_padding`: a layout that leaves a gap before any of its members is an error wherever it is placed.
    bool is_no_padding = false;
    /// The element of a `buffer` or an `array`; `none` for every other kind.
    type_id element = type_id::none;
    /// An `array`'s length; 0 for `T[]`, whose length the host binds (CHK-286).
    i32 count = 0;
    /// Whether a `buffer` may be written: `mut buffer[T]` against `buffer[T]`.
    bool is_mut = false;
    /// The shape of a `texture` or an `image`.
    texture_shape shape = {};
    /// A `texture` that holds depth, which takes no `element`.
    bool is_depth = false;
    /// A position in `k_image_formats` for an `image`; -1 for every other kind.
    i32 format = -1;
    access_mode access = access_mode::read;
    /// A `sampler` that compares.
    bool is_comparison = false;
    /// How a resource type is written, `out image_2d[.rgba8_unorm]`; empty for a declared type, which its symbol names.
    cc::string spelled;
    /// A generic struct of the prelude, `struct report[A]:`, whose `element` is its type parameter (CHK-339).
    bool is_template = false;
    /// An instance of one, `report[hit_attributes]`: the template, whose `element` this instance's argument replaces.
    type_id generic = type_id::none;

    bool operator==(type_info const&) const = default;
};

/// How a member that crosses from the vertex to the pixel stage is interpolated: `@interpolate(kind, sampling)` (CHK-273).
struct sgl::check::interpolation
{
    enum class kind_t : u8
    {
        perspective,
        linear,
        flat,
    };
    enum class sampling_t : u8
    {
        center,
        centroid,
        sample,
    };
    kind_t kind = kind_t::perspective;
    sampling_t sampling = sampling_t::center;

    constexpr bool operator==(interpolation const&) const = default;
};

/// What a member of a `@pixel struct` is: a color target, or an output that is none (CHK-276).
enum class sgl::check::pixel_output : sgl::u8
{
    color,
    /// `@depth`: the pixel's own depth, which it promises nothing about.
    depth,
    /// `@depth(.greater_equal)` and `@depth(.less_equal)`: a depth that only moves one way, which keeps early testing.
    depth_greater_equal,
    depth_less_equal,
    /// `@sample_mask`: which samples the pixel writes.
    sample_mask,
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
    /// `@interpolate(…)`, or the default: perspective at the pixel centre.
    interpolation interpolate;
    /// Carries `@interpolate` at all, which a member that is no stage link must not.
    bool has_interpolate = false;
    /// On a `@pixel struct`, whether the member is a color target or another output; `color` everywhere else.
    pixel_output output = pixel_output::color;
    /// `@edge_factors` or `@inside_factors`: a tessellation factor of a factors struct (CHK-305); `none` everywhere else.
    tessellation_factor factor = tessellation_factor::none;
    /// A `@vertex struct` member's `@format(.case)`: the `sg::vertex_attribute_format` its bytes are; empty for the one
    /// its type implies (CHK-275).
    cc::string vertex_format;
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
    /// `@sampler(name)` on a texture: the position among its binding's members of the sampler a sampling call without
    /// one reads (CHK-279); -1 without one.
    i32 default_sampler = -1;
    /// `@sampler(name)` on a texture that names a file-scope sampler rather than a member (CHK-279); none otherwise.
    /// At most one of it and `default_sampler` is set.
    symbol_id default_file_sampler = symbol_id::none;

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
    /// A file-scope `sampler`: a static sampler of the pipeline layout of every entry point that uses it (CHK-314).
    /// `info` is its position in `checked_module::samplers`, and `type` the sampler type its settings make.
    sampler,
    /// A named declaration this phase has no meaning for yet: `type`.
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

/// What a function is to the call model, which calls every one of them the same way (CHK-69).
enum class sgl::check::function_role : sgl::u8
{
    /// Declared at file scope, found by its name where it is visible.
    free,
    /// A `fun` of a type scope whose first parameter is `self` (CHK-234).
    method,
    /// A `fun` of a type scope without `self` (CHK-235).
    static_,
    /// `name => value`, a function of a type scope whose one parameter is `self` (CHK-236).
    property,
    /// The function of a struct's name that takes its fields (CHK-239); its declaration is the struct's.
    constructor,
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
    /// A struct's type, a const's, and a file-scope sampler's.
    type_id type = type_id::none;
    /// A position in `checked_module::functions`, `bindings`, `pipelines`, `constants` or `samplers`, by `kind`; -1
    /// before it is compiled.
    i32 info = -1;
    /// False under `@shadowable(false)`: a declaration or a local of its name is then an error rather than hiding it.
    bool is_shadowable = true;
    /// For a function: which kind of function it is.
    function_role role = function_role::free;
    /// The struct or enum whose type scope holds this function; `none` for a free function.
    /// A constructor's owner is its struct, although the constructor stands in the struct's scope and not in its own.
    symbol_id owner = symbol_id::none;

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
    /// In the file of the function; for a constructor, the struct field the parameter stands for.
    /// `none` for the `self` a property takes without writing it.
    ast::field_id field = ast::field_id::none;
    /// A call may leave the parameter out, and its default is then evaluated where the call stands (EVAL-81).
    bool has_default = false;
    /// Filled by name alone (CHK-244).
    bool is_named_only = false;
    /// The stage input its attribute marks it as, `none` for an ordinary parameter (CHK-271).
    stage_input input = stage_input::none;
    /// `p: mut T`: the caller's place, which a call hands over as `mut x` and the body may assign (CHK-315).
    bool is_mut = false;

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
    /// `@geometry(max_vertices = N)`'s `N`; 0 for every other stage.
    i32 max_vertices = 0;
    /// `@tessellation_control(partitioning = …, winding = …)`; the first of each for every other stage.
    tessellation_partitioning partitioning = tessellation_partitioning::integer;
    bool is_clockwise = true;
    /// The stages an entry point may be of to reach it, one bit per `stage` (`stage_bit`); every stage without `@stages`.
    u16 stages = k_every_stage;
    /// For an entry point, the features a device needs to run it: what it uses, never what it merely declares (CHK-263).
    /// Empty for every other function.
    feature_set features;
    /// `[A, B]`: a type of kind `type_parameter` each, in the order written; a range of `checked_module::type_lists`.
    ast::range_of<type_id> type_parameters;

    constexpr bool operator==(function_info const&) const = default;
};

struct sgl::check::binding_info
{
    symbol_id symbol = symbol_id::none;
    /// `@inline`: the members ride as inline constants, which an emitter must know.
    bool is_inline = false;
    /// `@workgroup`: the members are memory each workgroup of a dispatch shares, which no host binds (CHK-292).
    bool is_workgroup = false;
    /// `@no_padding`: a gap before any member of its constant block is an error.
    bool is_no_padding = false;
    ast::range_of<member_info> members;
    /// What its own `require` lines name, which declares them for every entry point listing it (CHK-262).
    feature_set declared;
    /// `declared` and whatever its members use: what every entry point listing it needs of a device (CHK-261).
    feature_set required;

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
    /// A `hit_group`: one row of a ray-tracing pipeline's table (CHK-330).
    hit_group,
};

/// A `pipeline` declaration that checked: its stages, its layout, and its configuration.
struct sgl::check::pipeline_info
{
    symbol_id symbol = symbol_id::none;
    pipeline_kind kind = pipeline_kind::raster;
    symbol_id vertex = symbol_id::none;
    /// `none` for a pipeline without a pixel stage, which writes depth alone.
    symbol_id pixel = symbol_id::none;
    /// Each `none` for a pipeline without it; the two tessellation stages are both `none` or neither (CHK-307).
    symbol_id geometry = symbol_id::none;
    symbol_id tessellation_control = symbol_id::none;
    symbol_id tessellation_evaluation = symbol_id::none;
    /// The binding layout: the longest binding list of its stages with `@inline` left out; a range of `binding_lists`.
    ast::range_of<symbol_id> layout;
    /// The one `@inline` binding its stages list, or `none`.
    symbol_id inline_constants = symbol_id::none;
    /// The vertex stage's parameter, and the pixel stage's result; `none` without a pixel stage.
    type_id vertex_input = type_id::none;
    type_id target_set = type_id::none;
    /// In the order they apply, each over the ones before it and all over sg's defaults.
    ast::range_of<pipeline_setting> settings;

    /// A ray-tracing pipeline's and a hit group's ray set, a `rays` declaration (CHK-330, CHK-331).
    symbol_id ray_set = symbol_id::none;
    symbol_id raygen = symbol_id::none;
    /// A ray-tracing pipeline's miss per ray type, each `none` for a ray type without one; a range of `binding_lists`.
    ast::range_of<symbol_id> misses;
    /// A ray-tracing pipeline's listed hit groups, in table order; a range of `binding_lists`.
    ast::range_of<symbol_id> hit_groups;
    /// Whether the host appends hit groups of its own after the listed ones: `.host`, last in `hit_groups`.
    bool has_host_hit_groups = false;
    /// Derived from the trace graph where every hit group is listed, and declared as a bound with `.host`.
    i32 max_recursion_depth = 0;
    /// A hit group's closest hit and any hit per ray type, two by two, each `none` where it has none.
    ast::range_of<symbol_id> records;
    symbol_id intersection = symbol_id::none;
    bool is_procedural = false;

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
    /// `self` in a method or a property, and so the object of a member a bare name reads through it (CHK-245).
    receiver,
    /// On a `member`: an array's `length`, a constant (CHK-288).
    array_length,
    /// On a call: `T[N].filled(v)`, an array holding `v` in every element (CHK-289).
    array_filled,
    /// On a call: the prelude's `undefined()`, a value of the type the parameter it meets has, which nobody reads
    /// (CHK-341).
    undefined_value,
};

/// What an expression refers to, for an editor: go to definition, hover, rename.
struct sgl::check::target
{
    target_kind kind = target_kind::none;
    symbol_id symbol = symbol_id::none;
    i32 index = -1;

    constexpr bool operator==(target const&) const = default;
};

/// `trace(world, r, set.ray, mut payload)`: a trace of a ray-tracing pipeline's ray type (CHK-329).
struct sgl::check::ray_trace
{
    i32 file = 0;
    ast::expr_id call = ast::expr_id::none;
    /// The ray set, a `rays` declaration.
    symbol_id set = symbol_id::none;
    /// The ray type's position in its set, which is the trace's ray contribution and its miss index.
    i32 ray = 0;
    /// The function the trace stands in, whose ray type it is a trace graph's edge from.
    symbol_id caller = symbol_id::none;

    constexpr bool operator==(ray_trace const&) const = default;
};

/// One argument a call wrote, in the order it wrote them, which is the order they are evaluated in (EVAL-80).
struct sgl::check::written_argument
{
    /// In the file of the call.
    ast::expr_id expr = ast::expr_id::none;
    /// A splat is one written argument per field of its value; this is that field, and -1 for no splat.
    i32 splat_member = -1;
    /// `mut x`: the caller's place, for a `mut` parameter (AST-149, CHK-316).
    bool is_mut = false;
    /// A function handed to a parameter of function type: a function's name, an arrow lambda, or such a parameter
    /// handed on (CHK-318); what it stands for is its expression's target.
    bool is_function = false;

    constexpr bool operator==(written_argument const&) const = default;
};

/// How one call's written arguments fill the parameters of the function resolution chose.
struct sgl::check::call_record
{
    symbol_id callee = symbol_id::none;
    ast::range_of<written_argument> written;
    /// One per parameter of `callee`: a position in `written`, or -1 where the parameter takes its default.
    ast::range_of<i32> slots;
    /// What a generic callee's type parameters stand for at this call, two by two: a parameter, then its argument
    /// (CHK-340); a range of `checked_module::type_lists`.
    ast::range_of<type_id> type_arguments;

    constexpr bool operator==(call_record const&) const = default;
};

/// Why a candidate did not take a call's arguments (CHK-252, CHK-70), or `none` where it did.
enum class sgl::check::miss_reason : sgl::u8
{
    none,
    no_such_parameter,
    filled_twice,
    positional_out_of_slot,
    positional_to_named_only,
    too_many,
    missing_argument,
    /// Every argument bound, and `argument` does not convert to `parameter`.
    no_conversion,
    /// `argument` is marked `mut` and `parameter` is no `mut` parameter, or the reverse (CHK-316).
    mut_mismatch,
};

/// One candidate of a call that matched nothing, and why: what a "did you mean" is written from.
struct sgl::check::near_miss
{
    i32 file = 0;
    ast::expr_id call = ast::expr_id::none;
    symbol_id candidate = symbol_id::none;
    miss_reason reason = miss_reason::none;
    /// A position in the call's written arguments, and one in the candidate's parameters; -1 where it is about neither.
    i32 argument = -1;
    i32 parameter = -1;

    constexpr bool operator==(near_miss const&) const = default;
};

/// The side tables over one file's untouched AST, each parallel to `file_ast::exprs`.
struct sgl::check::file_tables
{
    /// `none` for an expression nothing checked; an expression in a type position has the type it names.
    cc::vector<type_id> type_of;
    cc::vector<target> target_of;
    /// A position in `checked_module::call_records` for a call that resolved, -1 for every other expression.
    cc::vector<i32> call_of;

    [[nodiscard]] type_id type_at(ast::expr_id id) const { return type_of[ast::index_of(id)]; }
    [[nodiscard]] target const& target_at(ast::expr_id id) const { return target_of[ast::index_of(id)]; }
    [[nodiscard]] i32 call_at(ast::expr_id id) const { return call_of[ast::index_of(id)]; }

    [[nodiscard]] bool operator==(file_tables const& rhs) const
    {
        return ast::impl::is_equal(type_of, rhs.type_of) && ast::impl::is_equal(target_of, rhs.target_of)
            && ast::impl::is_equal(call_of, rhs.call_of);
    }
};
