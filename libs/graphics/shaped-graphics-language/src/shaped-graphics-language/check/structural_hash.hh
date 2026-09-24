#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/checked_module.hh>

/// A type's *shape*: what host code built against it depends on, and nothing else.
///
/// Two structs of one shape have the same members in the same order, each of the same name and shape, carrying the
/// same `@position`, `@thread_id`, `@per_instance` and `@stream`.
/// The type's own name is not part of it, so a rename is not a change of shape.
/// A builtin or an enum is its name, and an enum its cases too; a buffer is its element's shape and whether it is `mut`.
///
/// Stable for one compiler across runs and machines, which is what lets a build bake it and a hot reload compare against it.
namespace sgl::check
{
[[nodiscard]] cc::hash128 structural_hash(checked_module const& m, type_id type);

/// The shape of a member list: a struct's fields or a binding's members.
[[nodiscard]] cc::hash128 structural_hash(checked_module const& m, cc::span<member_info const> members);

/// 32 lowercase hex digits, high limb first: how `describe` writes a shape.
[[nodiscard]] cc::string hex_of(cc::hash128 hash);
} // namespace sgl::check
