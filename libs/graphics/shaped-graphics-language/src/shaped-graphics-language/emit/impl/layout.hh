#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/checked_module.hh>

/// Where a value lands in GPU memory: one rule per address space, the same on every target.
/// A target whose own rule would put a member elsewhere is made to follow it by its emitter; see the spec's layout section.

namespace sgl::emit::impl
{
/// The memory a value lives in, which decides the rule that places it.
enum class address_space : u8
{
    /// A group's constant block or the `@inline` block: HLSL's constant-buffer packing.
    constants,
    /// The elements of a `buffer[T]`: dx12's structured-buffer packing, each value aligned to its scalar's size.
    storage,
    /// A constant block marked `@layout(.cpp)`: as a C++ compiler places a struct of the generated host types (EMIT-154).
    /// Every host type aligns to its scalar, `tg::vec3f` is 12 bytes at 4, so this places as `storage` does.
    cpp_constants,
};

/// "constant block" or "storage buffer", for a diagnostic.
[[nodiscard]] cc::string_view space_name(address_space space);

/// The rule the constant block of `b` is placed by: `cpp_constants` under `@layout(.cpp)`, and `constants` otherwise.
[[nodiscard]] address_space block_space(check::binding_info const& b);

/// One builtin value inside a placed value: where it lands, from the start of the outermost one.
struct placed_leaf
{
    /// The member positions from the outermost value down, each in its struct's declared members.
    /// Empty for a value that is a builtin itself.
    cc::vector<i32> path;
    check::type_id type = check::type_id::none;
    i32 offset = 0;
    i32 size = 0;
};

/// Members placed one after another: a block, or the fields of a struct.
struct placed_members
{
    /// Parallel to the members; a void member has offset and size 0 and takes no room.
    cc::vector<i32> offsets;
    cc::vector<i32> sizes;
    /// Where the last member ends, which is the size of a struct in a constant block.
    /// In a buffer it is rounded up to `alignment`, as dx12 sizes a struct.
    i32 size = 0;
    /// The largest scalar size among the members: 4, or 2 where every value is 16 bits.
    i32 alignment = 2;
    /// Every builtin value inside, in memory order.
    cc::vector<placed_leaf> leaves;
};

/// The bytes of one scalar of a builtin: 2 for a 16-bit family, and 4 for every other.
[[nodiscard]] i32 scalar_size_of(builtins::type_record const& record);

/// Whether a value of `type` can stand in GPU memory: a builtin with a size there, or a struct of such values.
[[nodiscard]] bool is_placeable(check::checked_module const& m, check::type_id type);

/// The first value on the way into `type` that cannot stand in GPU memory, `light.flag: bool`, or empty where none.
[[nodiscard]] cc::string first_unplaceable(check::checked_module const& m, check::type_id type);

/// `members` placed by the rule of `space`; every member must be placeable, or void.
[[nodiscard]] placed_members place(check::checked_module const& m,
                                   cc::span<check::member_info const> members,
                                   address_space space);

/// The members of the struct `type` placed by the rule of `space`, from offset 0.
[[nodiscard]] placed_members place_struct(check::checked_module const& m, check::type_id type, address_space space);

/// The bytes one element of a `buffer[T]` of `element` takes: a builtin's size, or the struct's.
[[nodiscard]] i32 element_stride(check::checked_module const& m, check::type_id element);

/// Where padding stands among `members` placed in `space`: one line per member that starts past where the one before
/// it ended, `'uv' starts at byte 16, 4 bytes past where 'dir' ends`.
/// Only this level: a nested struct's own members are its declaration's to judge.
/// What follows the last member is never counted.
[[nodiscard]] cc::vector<cc::string> padding_of(check::checked_module const& m,
                                                cc::span<check::member_info const> members,
                                                address_space space);

/// Every struct `type` holds, itself included when it is one, each once, innermost first.
void collect_structs(check::checked_module const& m, check::type_id type, cc::vector<check::type_id>& out);

/// Every struct the binding `binding` places in `space`: through its constant block, or through its buffers' elements.
void collect_placed_structs(check::checked_module const& m,
                            check::symbol_id binding,
                            address_space space,
                            cc::vector<check::type_id>& out);
} // namespace sgl::emit::impl
