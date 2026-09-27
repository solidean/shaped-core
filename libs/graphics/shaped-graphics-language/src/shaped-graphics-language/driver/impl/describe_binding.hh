#pragma once

#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/driver/describe.hh>

namespace sgl::driver::impl
{
/// What `sgl describe` says of one binding: its members, their slots and facts, and its block.
/// `s` must be a binding that passed `emit::impl::validate_binding`.
[[nodiscard]] described_binding describe_binding(check::checked_module const& m, check::symbol const& s);
} // namespace sgl::driver::impl
