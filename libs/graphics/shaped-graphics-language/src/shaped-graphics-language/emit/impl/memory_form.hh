#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/emit/emit.hh>
#include <shaped-graphics-language/emit/impl/layout.hh>

/// How WGSL and MSL are made to follow SGL's layout where their own rules would place a member elsewhere.
///
/// A memory root is a constant block or the element of a buffer.
/// Where the target's own rule lands every value of a root at SGL's offset, the root is written as the program's structs
/// are, and nothing here applies to it.
/// Otherwise the root is written as its *memory form*: one flat struct of its builtin values in memory order, each
/// declared as the target can place it at SGL's offset, with padding between them.
/// A value that fits is declared as itself; a vector that does not is split into scalars (WGSL) or packed (MSL), and a
/// matrix into its scalars.
/// A read rebuilds the value from its pieces, and a write stores each piece; nothing of it reaches the program.

namespace sgl::emit::impl
{
/// One field of a memory form's struct.
struct memory_field
{
    cc::string name;
    /// As the target spells it: `f32`, `vec2f`, `packed_float3`.
    cc::string type;
    i32 offset = 0;
};

/// One builtin value of the root and the fields that hold it.
struct memory_leaf
{
    /// As `placed_leaf::path`: member positions from the root down.
    cc::vector<i32> path;
    check::type_id type = check::type_id::none;
    /// Positions in `memory_form::fields`: one where the value is a field of its own type, one per scalar where it is
    /// split.
    cc::vector<i32> fields;
    /// The fields are the value's scalars, in order, rather than the value itself.
    bool is_split = false;
    /// The field is MSL's packed spelling of the value, which a read converts back.
    bool is_packed = false;
};

/// A root written as its memory form.
struct memory_form
{
    /// The struct the target declares for it, minted.
    cc::string name;
    cc::vector<memory_field> fields;
    cc::vector<memory_leaf> leaves;
};

/// The memory form of `members` placed in `space` for target `t`, or nothing where `t`'s own rule already places every
/// value there, which is also the answer for both HLSL targets.
/// `stride` is the bytes one element takes where the root is a buffer's element, and 0 for a block.
[[nodiscard]] cc::optional<memory_form> memory_form_of(check::checked_module const& m,
                                                       cc::span<check::member_info const> members,
                                                       address_space space,
                                                       i32 stride,
                                                       target t);

/// The memory form of one element of a `buffer[element]` for target `t`, or nothing where `t` places it as SGL does.
[[nodiscard]] cc::optional<memory_form> element_form_of(check::checked_module const& m, check::type_id element, target t);
} // namespace sgl::emit::impl
