#pragma once

#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/driver/describe.hh>

namespace sgl::driver::impl
{
/// What `sgl describe` says of one binding: its members, their slots and facts, and its block.
/// `s` must be a binding that passed `emit::impl::validate_binding`.
[[nodiscard]] described_binding describe_binding(check::checked_module const& m, check::symbol const& s);

/// What `sgl describe` says of one file-scope sampler, which must have checked.
[[nodiscard]] described_file_sampler describe_file_sampler(check::checked_module const& m, check::symbol_id id);

/// The file-scope samplers the code of `legal` reaches, in index order.
[[nodiscard]] cc::vector<check::symbol_id> file_samplers_of(check::flat_entry_point const& legal);
} // namespace sgl::driver::impl
