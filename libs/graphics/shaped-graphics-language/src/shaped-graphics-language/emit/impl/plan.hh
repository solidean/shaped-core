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

/// The one `@inline binding` of the entry point.
struct planned_constants
{
    check::symbol_id symbol = check::symbol_id::none;
    /// The global a member is read through.
    cc::string name;
    /// The struct type of the block, which SGL has no name for, so it is minted.
    cc::string block_name;
    cc::vector<planned_member> members;
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
    cc::optional<planned_constants> constants;
    /// Parallel to `e.locals`.
    cc::vector<cc::string> locals;
    /// Holds every name above and every reserved word of the target; a writer mints what it still needs from here.
    check::name_mint names;
};

/// The column of a builtin's record `t` reads; the two HLSL targets share one.
[[nodiscard]] builtins::language language_of(target t);

/// True for a type the prelude declares `@builtin`, which a target spells in its own way and never declares.
[[nodiscard]] bool is_builtin_type(check::checked_module const& m, check::type_id type);

/// Appends what keeps `e` from being written, which is the same for every target.
void validate(check::checked_module const& m, check::flat_entry_point const& e, cc::vector<error>& errors);

/// `e` must have passed `validate`.
[[nodiscard]] plan make_plan(check::checked_module const& m, check::flat_entry_point const& e, target t);
} // namespace sgl::emit::impl
