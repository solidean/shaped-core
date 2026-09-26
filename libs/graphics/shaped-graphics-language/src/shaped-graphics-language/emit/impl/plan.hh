#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/emit/emit.hh>

/// Everything about one entry point that is decided before a line of text exists: what is declared, under which name, at which address.
/// A writer reads the plan and the flat tree, and decides nothing but spelling.

namespace sgl::emit::impl
{
/// How many groups an entry point may list besides its `@inline` binding, `sg::max_binding_groups`.
/// sgl does not link sg, so the number is repeated here, and the pipeline layout sg builds is what it has to match.
inline constexpr auto k_max_groups = 3;

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
};

struct planned_member
{
    /// As the text spells it; differs from `source_name` when that is reserved in this target.
    cc::string name;
    /// As the program spells it, which is what a semantic derives from, so that it is the same in every target.
    cc::string source_name;
    check::type_id type = check::type_id::none;
    bool is_position = false;
    /// The position among the members without `@position`; -1 on a `plain` struct and on the position itself.
    i32 location = -1;
    /// The byte offset in an inline binding's block; -1 everywhere else.
    i32 offset = -1;
};

struct planned_struct
{
    check::type_id type = check::type_id::none;
    cc::string name;
    struct_role role = struct_role::plain;
    cc::vector<planned_member> members;
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
    i32 group = 0;
    i32 slot = 0;
};

struct plan
{
    check::checked_module const& m;
    check::flat_entry_point const& e;
    target which;

    /// The program's structs the entry point needs, each after every struct it holds.
    cc::vector<planned_struct> structs;
    /// Parallel to `m.types`: a position in `structs`, or -1 for a builtin type and for a type nothing here needs.
    cc::vector<i32> struct_of_type;
    /// The enums the entry point mentions, in the order they were first needed.
    cc::vector<planned_enum> enums;
    /// Parallel to `m.types`: a position in `enums`, or -1 for a type that is no enum this entry point needs.
    cc::vector<i32> enum_of_type;
    /// What this target declares the entry point as: the source's name, or a minted one where the target reserves it.
    cc::string entry_name;
    cc::optional<planned_constants> constants;
    /// The constant blocks of the entry point's groups, one per group with a plain member, in group order.
    cc::vector<planned_constants> group_blocks;
    /// The resources the entry point's bindings declare, in group then slot order.
    cc::vector<planned_resource> resources;
    /// Parallel to `e.locals`.
    cc::vector<cc::string> locals;
    /// A compute entry point's parameter as the dispatch hands it over, unsigned, ahead of the `int3` the body reads;
    /// minted, so no local of the program can take it.
    cc::string dispatch_name;
    /// Holds every name above and every reserved word of the target; a writer mints what it still needs from here.
    check::name_mint names;
};

/// The position in `resources` of the resource `binding.member` names, or -1 where that member is no resource.
[[nodiscard]] i32 resource_of(plan const& p, check::symbol_id binding, i32 member);

/// The block `binding` is read through: the `@inline` one, or its group's; null for a group with no plain member.
[[nodiscard]] planned_constants const* block_of(plan const& p, check::symbol_id binding);

/// The members of a binding that are values rather than resources, which is every member of an `@inline` one.
[[nodiscard]] cc::vector<check::member_info> plain_members_of(check::checked_module const& m,
                                                              check::binding_info const& b);

/// The slot a group's first resource takes: 1 behind a constant block, which takes 0, and 0 without one.
[[nodiscard]] i32 first_resource_slot(check::checked_module const& m, check::binding_info const& b);

/// How the target of `p` spells the builtin type named `name`, such as the texel of an image format.
[[nodiscard]] cc::string_view builtin_spelling(plan const& p, cc::string_view name);
/// The column of a builtin's record `t` reads; the two HLSL targets share one.
[[nodiscard]] builtins::language language_of(target t);

/// True for a type the prelude declares `@builtin`, which a target spells in its own way and never declares.
[[nodiscard]] bool is_builtin_type(check::checked_module const& m, check::type_id type);

/// Appends what keeps `e` from being written, which is the same for every target.
void validate(check::checked_module const& m, check::flat_entry_point const& e, cc::vector<error>& errors);

/// The buffer a vertex input member is read from (EMIT-92): its `@stream`, else `per_instance` or `per_vertex`.
[[nodiscard]] cc::string stream_of(check::member_info const& member);

/// Appends what keeps a struct from standing at one edge of the pipeline in `role`, whichever entry point uses it.
void validate_edge_struct(check::checked_module const& m, check::type_id type, struct_role role, cc::vector<error>& errors);

/// Appends what keeps the binding `id` from being written, whichever entry point lists it.
/// What only a list can get wrong, an `@inline` binding that does not stand last, is `validate`'s.
void validate_binding(check::checked_module const& m, check::symbol_id id, cc::vector<error>& errors);

/// Where the members of an `@inline` block land, which is the same in every target or `validate_binding` refused it.
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
[[nodiscard]] block_placement place_block(check::checked_module const& m, cc::span<check::member_info const> members);

/// `e` must have passed `validate`.
[[nodiscard]] plan make_plan(check::checked_module const& m, check::flat_entry_point const& e, target t);
} // namespace sgl::emit::impl
