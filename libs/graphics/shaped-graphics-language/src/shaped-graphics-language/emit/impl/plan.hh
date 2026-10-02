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
};

struct planned_member
{
    /// As the text spells it; differs from `source_name` when that is reserved in this target.
    cc::string name;
    /// As the program spells it, which is what a semantic derives from, so that it is the same in every target.
    cc::string source_name;
    check::type_id type = check::type_id::none;
    bool is_position = false;
    /// How a stage link's member is interpolated; the default everywhere else.
    check::interpolation interpolate;
    /// A render target struct's depth or sample mask, which takes no location; `color` everywhere else.
    check::pixel_output output = {};
    /// A factors struct's tessellation factors, which take no location; `none` everywhere else.
    check::tessellation_factor factor = {};
    /// The position among the members without `@position`; -1 on a `plain` struct and on the position itself.
    i32 location = -1;
    /// The byte offset in a constant block; -1 everywhere else.
    i32 offset = -1;
};

struct planned_struct
{
    check::type_id type = check::type_id::none;
    cc::string name;
    struct_role role = struct_role::plain;
    /// Only the members a target writes: a void one holds nothing, so no target declares it (EMIT-106).
    cc::vector<planned_member> members;
    /// Parallel to the type's members: the position in `members`, or -1 for a void member.
    cc::vector<i32> member_of;
};

/// An enum the entry point mentions, whose every case is declared whether or not an arm names it (EMIT-77).
struct planned_enum
{
    check::type_id type = check::type_id::none;
    /// Parallel to the type's cases: the constant each is written as, minted from `<enum>_<case>`.
    cc::vector<cc::string> case_names;
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
    bool is_mut = false;
    /// `@coherent`: HLSL declares it `globallycoherent`, and MSL `coherent(device)` (EMIT-150).
    bool is_coherent = false;
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
    /// Holds every name above and every reserved word of the target; a writer mints what it still needs from here.
    check::name_mint names;
};

/// The position in `workgroup` of `binding.member`, or -1 where that member is no workgroup memory.
[[nodiscard]] i32 workgroup_of(plan const& p, check::symbol_id binding, i32 member);
/// The position in `resources` of the resource `binding.member` names, or -1 where that member is no resource.
[[nodiscard]] i32 resource_of(plan const& p, check::symbol_id binding, i32 member);

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
void validate(check::checked_module const& m, check::flat_entry_point const& e, cc::vector<error>& errors);

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

/// Where the members of a constant block land, by the rule its binding's `@layout` names (layout.hh).
struct block_placement
{
    /// Parallel to the members.
    cc::vector<i32> offsets;
    /// Parallel to the members.
    cc::vector<i32> sizes;
    /// Where the last member ends.
    i32 size = 0;
};

/// `members` must belong to a binding that passed `validate_binding`.
[[nodiscard]] block_placement place_block(check::checked_module const& m,
                                          cc::span<check::member_info const> members,
                                          address_space space);

/// Every constant block and buffer element of `p`, as its target's text declares it.
[[nodiscard]] cc::vector<emitted_layout> layouts_of(plan const& p);

/// `e` must have passed `validate`.
[[nodiscard]] plan make_plan(check::checked_module const& m, check::flat_entry_point const& e, target t);
} // namespace sgl::emit::impl
