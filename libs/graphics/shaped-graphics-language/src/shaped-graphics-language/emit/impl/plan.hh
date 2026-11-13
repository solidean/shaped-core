#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/emit/emit.hh>
#include <shaped-graphics-language/emit/impl/memory_form.hh>

/// Everything about one entry point that is decided before a line of text exists: what is declared, under which name, at which address.
/// A writer reads the plan and the flat tree, and decides nothing but spelling.

namespace sgl::emit::impl
{
/// How many groups an entry point may list besides its `@inline` binding, `sg::max_binding_groups`.
/// sgl does not link sg, so the number is repeated here, and the pipeline layout sg builds is what it has to match.
inline constexpr auto k_max_groups = 3;
/// The space slib's binding pass gives the inline constants of a dx12 pipeline, `slib::inline_constants_space`.
/// sgl does not link slib, so the number is repeated here, and the pipeline layout sg builds is what it has to match.
inline constexpr auto k_inline_constants_space = 9;
/// The space slib gives a pipeline layout's static samplers on dx12, `slib::bound_samplers_space`, repeated for the same reason.
inline constexpr auto k_bound_samplers_space = 10;

/// How many file-scope sampler indices a stage may reach.
/// Metal's argument table and WebGPU's default `maxSamplersPerShaderStage` both hold 16.
inline constexpr auto k_max_file_samplers = 16;

/// What a struct is to the entry point, which decides how its members are addressed.
/// It comes from where the struct stands in the signature, never from the struct's own attribute.
enum class struct_role : u8
{
    /// A value like any other: its members carry no address.
    plain,
    /// The parameter of a vertex entry point: member i is vertex attribute i.
    vertex_input,
    /// What one stage hands to the next: the result of a vertex entry point and the parameter of a pixel one.
    stage_link,
    /// The result of a pixel entry point: member i is render target i.
    render_targets,
    /// What a tessellation control stage returns and the evaluation stage takes: its factors, and data per patch.
    patch_constants,
    /// What a mesh stage writes per triangle and the pixel stage takes beside its stage link, flat (CHK-488).
    primitive_link,
};

struct planned_member
{
    /// As the text spells it; differs from `source_name` when that is reserved in this target.
    cc::string name;
    /// As the program spells it, which is what a semantic derives from, so that it is the same in every target.
    cc::string source_name;
    check::type_id type = check::type_id::none;
    bool is_position = false;
    /// `@layer`, which the target names by an attribute of its own and no location (CHK-419).
    bool is_layer = false;
    /// How a stage link's member is interpolated; the default everywhere else.
    check::interpolation interpolate;
    /// A render target struct's depth or sample mask, which takes no location; `color` everywhere else.
    check::pixel_output output = {};
    /// A factors struct's tessellation factors, which take no location; `none` everywhere else.
    check::tessellation_factor factor = {};
    /// The position among the members without `@position`; -1 on a `plain` struct and on the position itself.
    i32 location = -1;
    /// The byte offset in a constant block or a buffer's element; -1 everywhere else.
    i32 offset = -1;
    /// The bytes it takes there, where `offset` is one.
    i32 size = 0;
    /// `@padding(.every_element)`: a constant block's array of values, each element on a row, held as 4-vectors (EMIT-172).
    bool is_row_per_element = false;
    /// An edge struct's array or matrix, which crosses as `pieces` members of `piece`, its elements or its columns, at
    /// consecutive locations from `location` (EMIT-174); 0 for any other member.
    i32 pieces = 0;
    check::type_id piece = check::type_id::none;
    /// The locations each piece takes: two for a 64-bit three- or four-vector the target carries as itself (CHK-454).
    i32 piece_locations = 1;
    /// In `declared_members`: where the member stands in its struct's `members`, and which of its pieces this is, or -1.
    isize whole = -1;
    i32 piece_index = -1;
};

struct planned_struct
{
    /// Placed in a constant block rather than in a buffer, which is where HLSL holds a `mat2` as a `float4` (EMIT-175).
    bool is_constants = false;
    check::type_id type = check::type_id::none;
    cc::string name;
    struct_role role = struct_role::plain;
    /// Only the members a target writes: a void one holds nothing, so no target declares it (EMIT-106).
    cc::vector<planned_member> members;
    /// Parallel to the type's members: the position in `members`, or -1 for a void member.
    cc::vector<i32> member_of;
    /// Its size in GPU memory, its tail padding included; -1 where it stands in none.
    i32 memory_size = -1;
    /// It stands in GPU memory under `@padding(.none)`.
    bool is_packed = false;
    /// Its alignment in GPU memory, where it stands there.
    i32 memory_alignment = 0;
};

/// An enum the entry point mentions, whose every case is declared whether or not an arm names it (EMIT-77).
struct planned_enum
{
    check::type_id type = check::type_id::none;
    /// Parallel to the type's cases: the constant each is written as, minted from `<enum>_<case>`.
    cc::vector<cc::string> case_names;
};

/// What a target holds in GPU memory where SGL's value is, which a read converts back and a write converts into (EMIT-173).
enum class held_as : u8
{
    itself,
    /// HLSL's `float4x3` of a `mat3` (EMIT-168): a read narrows it, a write widens into it.
    widened_mat3,
    /// MSL's `packed_` 3-vector (EMIT-170): a read converts it, and a write converts into it implicitly.
    packed_tail,
    /// An array of 4-vectors (EMIT-169, EMIT-172): an element reads through `lanes`, a whole read or write goes element by
    /// element.
    widened_array,
    /// A `mat2` in an HLSL constant block, held as one `float4` of its two columns (EMIT-175): a read rebuilds it.
    vector_mat2,
    /// An edge struct's array or matrix, held as one member per element or column (EMIT-174): a read rebuilds it, a write
    /// stores each piece.
    pieces,
};

/// How one expression reaching GPU memory is spelled, which `plan_memory_accesses` decides before any text is written.
struct memory_access
{
    held_as held = held_as::itself;
    /// The lanes an element of a widened array is read and written through: `.x`, `.xy` or `.xyz`.
    cc::string_view lanes;
    /// Spelled as held, never converted: an assignment's place, the objects a write through it reaches, or an array
    /// indexed in place.
    bool is_raw = false;
};

/// A block of constants: the one `@inline binding` of the entry point, or the plain members of one of its groups.
struct planned_constants
{
    check::symbol_id symbol = check::symbol_id::none;
    /// The global a member is read through, minted like any other name.
    cc::string name;
    /// What the host binds the block by: the binding's own name.
    cc::string host_name;
    /// The struct type of the block, which SGL has no name for, so it is minted.
    cc::string block_name;
    /// The plain members only, each at the offset `place_block` gave it.
    cc::vector<planned_member> members;
    /// Parallel to the binding's members: a position in `members`, or -1 for a buffer.
    cc::vector<i32> block_member_of;
    /// A group's block is the first resource of its group, at slot 0; -1 for the `@inline` block, which takes no group.
    i32 group = -1;
    i32 slot = -1;
    /// The block as its target has to declare it to reach SGL's offsets, named `block_name`; absent where it declares
    /// the members as they are.
    cc::optional<memory_form> form;
    /// A reserved binding's block, at the reserved table's address, which reaches no host list and no layout.
    bool is_reserved = false;
};

/// A resource member of a binding — a buffer, a texture, an image or a sampler — rather than a field of a block.
/// Its address is the group its binding is listed at and the slot it takes among that binding's resources.
struct planned_resource
{
    check::symbol_id binding = check::symbol_id::none;
    /// A position in the binding's `members`.
    i32 member = -1;
    /// The global the shader reads and writes through, minted like any other name.
    cc::string name;
    /// What the host binds the resource by: `binding.member`, which no target can spell.
    cc::string host_name;
    /// The member's type, whose kind says which resource it is.
    check::type_id type = check::type_id::none;
    /// The element of a buffer; `none` for every other kind.
    check::type_id element = check::type_id::none;
    /// The rule a buffer's elements are placed by, which `@padding(.none)` makes the packed one.
    address_space space = address_space::storage;
    bool is_mut = false;
    /// `@coherent`: HLSL declares it `globallycoherent`, and MSL `coherent(device)` (EMIT-150).
    bool is_coherent = false;
    /// An entry of `k_reserved_bindings`: sg binds it, so the host never hears of it, and it stands at its fixed address.
    bool is_reserved = false;
    i32 group = 0;
    /// The first of the `count` consecutive slots it takes: a binding array takes one per element (CHK-299).
    i32 slot = 0;
    /// A binding array's length; 1 for any other resource.
    i32 count = 1;
    /// A buffer's element as its target has to declare it to reach SGL's stride and offsets; absent where the element
    /// type as it is does.
    cc::optional<memory_form> element_form;
};

/// A file-scope sampler the entry point's code reaches, a static sampler of its pipeline layout (EMIT-133).
struct planned_sampler
{
    check::symbol_id symbol = check::symbol_id::none;
    /// The global the shader samples through, minted like any other name.
    cc::string name;
    /// What the host binds it by, which is the sampler's own name.
    cc::string host_name;
    check::type_id type = check::type_id::none;
    /// Its position among the module's file-scope samplers, which is the same in every entry point and every stage.
    i32 index = 0;
};

/// One member of a `@workgroup` binding: memory the workgroup shares, which no host binds.
struct planned_workgroup
{
    check::symbol_id binding = check::symbol_id::none;
    /// A position in the binding's `members`.
    i32 member = -1;
    /// The variable the shader reads and writes through, minted like any other name.
    cc::string name;
    check::type_id type = check::type_id::none;
};

/// One group as MSL passes it: an argument buffer the entry point takes at `[[buffer(group)]]`.
struct planned_argument_buffer
{
    i32 group = 0;
    /// The struct of the group's slots, minted from `<binding>_arguments`.
    cc::string struct_name;
    /// The parameter it arrives through, minted from `<binding>_group`.
    cc::string parameter;
};

/// The ray set that sizes a ray-tracing entry point's ray data on MSL, `none` for one that has none (EMIT-139).
/// It is the set of the first pipeline or hit group holding the entry point, else the set it traces or is handed a
/// payload of.
[[nodiscard]] check::symbol_id ray_set_of(check::checked_module const& m, check::flat_entry_point const& e);
/// The ray sets of every ray-tracing pipeline and hit group holding `e` as one of its shaders, each once.
[[nodiscard]] cc::vector<check::symbol_id> owning_ray_sets(check::checked_module const& m,
                                                           check::flat_entry_point const& e);

struct plan
{
    check::checked_module const& m;
    check::flat_entry_point const& e;
    target which;
    /// Every `atomic[float]` is the target's own float atomic, rather than a `uint`'s bits: the device adds floats in one
    /// instruction, and the target can say so (vulkan, metal).
    bool native_float_atomics = false;
    /// The text writes prints and asserts into sg's debug window (EMIT-183), which a metal raygen hands every stage it
    /// calls through its context, whether it prints itself or not.
    bool diagnostics = false;
    /// MSL: the package takes `SOFTWARE_TRACE`, so a structure that moves may be bound, and a native trace takes motion tags.
    bool sees_motion = false;
    /// The target and the device the compile writes for, which a preamble's condition reads (CHK-528).
    check::compile_target compiled_for;
    /// dx12: the compile fills the draw's start natively, so the text reads it as Shader Model 6.8's system values
    /// (EMIT-128); under any other fill no entry that takes the draw's start reaches the writer.
    bool reads_draw_start = false;
    /// dx12: the compile fills the subgroup size natively, so the text states a preferred one as a range (EMIT-148).
    bool states_wave_size_range = false;
    /// dx12: the entry point reorders through NVAPI's shader extensions, the `nvapi` fill of shader execution
    /// reordering (EMIT-263): its hit objects are `NvHitObject`s, which talk to the driver through `u0, space13`.
    bool reorders_through_nvapi = false;
    /// Under that fill, each `hit_object_invoke`'s hit object, the expression of its first argument, with the
    /// acceleration structure of the `trace_hit` that made it, which NVAPI invokes it through (EMIT-263).
    struct invoked_structure
    {
        check::flat_expr_id hit = check::flat_expr_id::none;
        check::flat_expr_id structure = check::flat_expr_id::none;
    };
    cc::vector<invoked_structure> invoked_structures;
    /// The entry point's traces see opacity micromaps: it holds the feature, and its fill writes them (EMIT-184).
    /// The `nvapi` fill writes nothing, since NVIDIA's driver applies them to every trace of a structure that has them.
    bool states_micromaps = false;

    /// The program's structs the entry point needs, each after every struct it holds.
    cc::vector<planned_struct> structs;
    /// Parallel to `m.types`: a position in `structs`, or -1 for a builtin type and for a type nothing here needs.
    cc::vector<i32> struct_of_type;
    /// How the target spells each array type the entry point needs: `array<f32, 5>`.
    /// HLSL's is its innermost element's, since HLSL writes the lengths after the name (`array_dimensions`).
    cc::vector<cc::string> array_texts;
    /// Parallel to `m.types`: a position in `array_texts`, or -1 for a type that is no array this entry point needs.
    cc::vector<i32> array_of_type;
    /// The enums the entry point mentions, in the order they were first needed.
    cc::vector<planned_enum> enums;
    /// Parallel to `m.types`: a position in `enums`, or -1 for a type that is no enum this entry point needs.
    cc::vector<i32> enum_of_type;
    /// What this target declares the entry point as: the source's name, or a minted one where the target reserves it.
    cc::string entry_name;
    /// A tessellation control stage's patch-constant function, which HLSL writes its body as, and the index of the
    /// control point its pass-through hull function hands on; empty for every other stage.
    cc::string patch_function;
    cc::string point_index;
    cc::optional<planned_constants> constants;
    /// The constant blocks of the entry point's groups, one per group with a plain member, in group order.
    cc::vector<planned_constants> group_blocks;
    /// The resources the entry point's bindings declare, in group then slot order.
    cc::vector<planned_resource> resources;
    /// The file-scope samplers its code reaches, in `index` order.
    cc::vector<planned_sampler> samplers;
    /// The members of its `@workgroup` bindings, in the order listed and then declared.
    cc::vector<planned_workgroup> workgroup;
    /// MSL's: one per group with a constant block or a resource, in group order; empty on every other target.
    cc::vector<planned_argument_buffer> argument_buffers;
    /// Parallel to `e.locals`.
    cc::vector<cc::string> locals;
    /// Parallel to `e.stage_inputs`: each as the target hands it over, unsigned, ahead of the local the body reads;
    /// minted, so no local of the program can take it.
    cc::vector<cc::string> stage_input_names;
    /// Parallel to `e.stage_inputs`: a second parameter where a target needs one, dx12's `SV_StartInstanceLocation`.
    cc::vector<cc::string> stage_input_bases;
    /// Parallel to `e.stage_inputs`: whether the tree reads it, which a target's line that converts it waits for.
    cc::vector<bool> stage_input_read;
    /// Parallel to `locals`: the constant flags every call with a `flags_argument` hands the local, which its type takes.
    /// Empty where no call does, where one hands it flags that are no constant, or where two hand it different ones.
    cc::vector<cc::optional<u32>> local_flags;
    /// Parallel to `e.exprs`: how each expression reaching GPU memory is spelled (EMIT-173).
    cc::vector<memory_access> accesses;
    /// Holds every name above and every reserved word of the target, and goes on from the tree's names and the builtins'.
    /// A writer mints what it still needs from here.
    check::name_mint names;
};

/// The position in `workgroup` of `binding.member`, or -1 where that member is no workgroup memory.
[[nodiscard]] i32 workgroup_of(plan const& p, check::symbol_id binding, i32 member);
/// The position in `resources` of the resource `binding.member` names, or -1 where that member is no resource.
/// The acceleration structure NVAPI invokes the hit object `hit` through, of `plan::invoked_structures`; none where it
/// has none.
[[nodiscard]] check::flat_expr_id invoked_structure_of(plan const& p, check::flat_expr_id hit);
/// The acceleration structure the `trace_hit` that made the hit object `hit` traced, under CHK-434's rule that a hit object
/// is the `let` its trace gave; or why there is no one trace, for a refusal to name.
/// The trace pins what it took in a constant, which the invocation names, so the expression need not stay one value.
[[nodiscard]] cc::result<check::flat_expr_id, cc::string> traced_structure(check::checked_module const& m,
                                                                           check::flat_entry_point const& e,
                                                                           check::flat_expr_id hit);
[[nodiscard]] i32 resource_of(plan const& p, check::symbol_id binding, i32 member);

/// Where vulkan's `sg_acceleration_addresses` holds the address of resource `index`, an acceleration member: its rank
/// among the plan's, group by group and each group's by slot, which is how sg numbers them.
[[nodiscard]] u32 acceleration_address_position(plan const& p, i32 index);

/// Whether the plan holds an acceleration member a trace reaches by address, which then declares the address block.
[[nodiscard]] bool holds_acceleration_member(plan const& p);

/// The position in `samplers` of the file-scope sampler `symbol`, or -1 where the entry point reaches none of it.
[[nodiscard]] i32 sampler_of(plan const& p, check::symbol_id symbol);
/// The position of the file-scope sampler `symbol` among the module's, in declaration order: its index in every layout.
[[nodiscard]] i32 file_sampler_index(check::checked_module const& m, check::symbol_id symbol);

/// The block `binding` is read through: the `@inline` one, or its group's; null for a group with no plain member.
[[nodiscard]] planned_constants const* block_of(plan const& p, check::symbol_id binding);

/// The members of a binding that are values rather than resources, which is every member of an `@inline` one.
[[nodiscard]] cc::vector<check::member_info> plain_members_of(check::checked_module const& m,
                                                              check::binding_info const& b);

/// The slot a group's first resource takes: 1 behind a constant block, which takes 0, and 0 without one.
[[nodiscard]] i32 first_resource_slot(check::checked_module const& m, check::binding_info const& b);

/// How the target of `p` spells the builtin type named `name`, such as the texel of an image format.
[[nodiscard]] cc::string_view builtin_spelling(plan const& p, cc::string_view name);
/// An atomic as the target spells it: HLSL's plain integer, which `Interlocked*` updates, and WGSL's and MSL's atomic types.
[[nodiscard]] cc::string_view atomic_text(plan const& p, check::type_id type);
/// What HLSL writes after a declared name of `type`, `[3][5]`; empty for any other type and any other target.
[[nodiscard]] cc::string array_dimensions(plan const& p, check::type_id type);
/// The column of a builtin's record `t` reads; the two HLSL targets share one.
[[nodiscard]] builtins::language language_of(target t);

/// True for a type the prelude declares `@builtin`, which a target spells in its own way and never declares.
[[nodiscard]] bool is_builtin_type(check::checked_module const& m, check::type_id type);

/// Appends what keeps `e` from being written, which is the same for every target.
/// `writes_prints` accepts a print, which only a text with diagnostics writes (EMIT-183).
void validate(check::checked_module const& m,
              check::flat_entry_point const& e,
              cc::vector<error>& errors,
              bool writes_prints = false);

/// How each target hands a stage input to an entry point (EMIT-127): its type there, and what marks it.
struct stage_input_spelling
{
    cc::string_view hlsl_type;
    cc::string_view hlsl_semantic;
    cc::string_view wgsl_type;
    cc::string_view wgsl_builtin;
    cc::string_view msl_type;
    cc::string_view msl_attribute;
    /// What HLSL reads instead of a parameter, for an input it has no semantic for: `WaveGetLaneCount()` (EMIT-147).
    cc::string_view hlsl_read = {};
};
[[nodiscard]] stage_input_spelling const& spelling_of(check::stage_input input);

/// True where the target counts `input` from the draw's base and the text adds the base back: HLSL's vertex and instance.
[[nodiscard]] bool has_base(plan const& p, check::stage_input input);

/// The line that turns a stage input as the target handed it over into the value the body reads: `int3(id_in)`.
/// `base` is the second parameter a target needs, or empty.
[[nodiscard]] cc::string stage_input_value(plan const& p, isize index);

/// The buffer a vertex input member is read from (EMIT-92): its `@stream`, else `per_instance` or `per_vertex`.
[[nodiscard]] cc::string stream_of(check::member_info const& member);
/// The dx12 semantic of each member of the `@vertex struct` `t`, parallel to its members (EMIT-28).
/// The emitted text and the host's input layout both take them from here, so the two always name a member alike.
[[nodiscard]] cc::vector<cc::string> vertex_semantics(check::checked_module const& m, check::type_info const& t);

/// Appends what keeps a struct from standing at one edge of the pipeline in `role`, whichever entry point uses it.
void validate_edge_struct(check::checked_module const& m, check::type_id type, struct_role role, cc::vector<error>& errors);

/// Appends what keeps the binding `id` from being written, whichever entry point lists it.
/// What only a list can get wrong, an `@inline` binding that does not stand last, is `validate`'s.
void validate_binding(check::checked_module const& m, check::symbol_id id, cc::vector<error>& errors);

/// Where the members of a constant block land, by the rule its binding's `@padding` names (layout.hh).
struct block_placement
{
    /// Parallel to the members.
    cc::vector<i32> offsets;
    /// Parallel to the members.
    cc::vector<i32> sizes;
    /// Where the last member ends.
    i32 size = 0;
};

/// EMIT-170: whether member `member` of `s`, a struct in natural GPU memory, is a 3-vector the next member sits in the
/// tail of, which MSL's 16-byte 3-vector would push past, so MSL declares it packed.
[[nodiscard]] bool shares_tail(plan const& p, planned_struct const& s, isize member);
/// The same of `members` placed in natural memory, a block's or a struct's.
[[nodiscard]] bool shares_tail(plan const& p, cc::span<planned_member const> members, isize member);

/// EMIT-174: the members of `s` as its target declares them, an array or a matrix of an edge struct as one member per
/// element or column, named `<member>_<i>` at consecutive locations.
[[nodiscard]] cc::vector<planned_member> declared_members(planned_struct const& s);

/// The semantic HLSL declares member `member` of `s` under, which is what a DXIL signature names it by on both routes;
/// empty for a member of a struct that crosses no edge.
/// SPIR-V has no semantics: vulkan takes the location as an attribute, and keeps this because HLSL's grammar asks for it.
[[nodiscard]] cc::string semantic_of(plan const& p, planned_struct const& s, planned_member const& member);

/// EMIT-174: whether `element` names one piece of an array held as pieces, by a literal index.
/// `p.accesses` must hold the element's object already.
[[nodiscard]] bool is_piece(plan const& p, check::flat_element const& element);

/// EMIT-196: whether a 64-bit value of `type` crosses a link as its 32-bit words, low word first, where the target
/// carries no 64-bit value between stages (`hlsl-dx12`, MSL).
[[nodiscard]] bool is_word_split(plan const& p, check::type_id type);
/// EMIT-196: the pieces a value of `type` crosses as where it is word split: one, or two for a three- or four-vector.
[[nodiscard]] i32 word_pieces(plan const& p, check::type_id type);
/// CHK-454: whether `type` is a 64-bit three- or four-vector, which takes two locations.
[[nodiscard]] bool is_wide_64_bit(plan const& p, check::type_id type);

/// EMIT-173: fills `p.accesses` from the target, its blocks and its structs in memory, and from where each expression
/// stands: a place, or an array indexed in place, is spelled as held, and every other read is converted back.
void plan_memory_accesses(plan& p);

/// What reaches a matrix from an element of `type`, a multi-dimensional array of mat3 in HLSL's memory: `.m` of its
/// `sgl_mat3_cell` (EMIT-168); empty for any other type.
[[nodiscard]] cc::string_view mat3_cell_field(plan const& p, check::type_id type);
/// Whether member `member` of `object_type` is a mat3 its target holds as a `float4x3`, where the struct is in memory.
[[nodiscard]] bool holds_widened_mat3(plan const& p, check::type_id object_type, isize member);
/// Whether member `member` of `object_type` is an array of 3-vectors its target holds as 4-vectors (EMIT-169).
[[nodiscard]] bool holds_widened_array(plan const& p, check::type_id object_type, isize member);

/// The prelude's vector of one 16-byte row of the scalar of `element`, `float4` for a `float2` and `long2` for a
/// `long`, which holds one element per row.
[[nodiscard]] cc::string_view row_vector_of(plan const& p, check::type_id element);

/// `members` must belong to a binding that passed `validate_binding`.
[[nodiscard]] block_placement place_block(check::checked_module const& m,
                                          cc::span<check::member_info const> members,
                                          address_space space);

/// Every constant block and buffer element of `p`, as its target's text declares it.
[[nodiscard]] cc::vector<emitted_layout> layouts_of(plan const& p);

/// `e` must have passed `validate`.
[[nodiscard]] plan make_plan(check::checked_module const& m,
                             check::flat_entry_point const& e,
                             target t,
                             bool native_float_atomic_add = false);
} // namespace sgl::emit::impl
