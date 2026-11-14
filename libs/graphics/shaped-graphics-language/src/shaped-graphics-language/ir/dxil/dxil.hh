#pragma once

#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/fwd.hh>

namespace sgl::ir
{
/// `p` as a DXIL container D3D12 can load, or the construct DXIL has no lowering for yet.
///
/// `p` must have been through `construct_ssa`, since DXIL takes no variable that is a whole vector.
/// Nothing here validates, but the container carries its own digest, as one a validator signed does.
[[nodiscard]] cc::optional<cc::string> write_dxil(program const& p, cc::vector<byte>& out);

/// The digest at bytes 4 to 20 of a DXBC container, over everything after it: what a validator writes when it signs, and
/// what D3D12 recomputes before it takes a shader.
/// An MD5 whose last block DXBC lays out its own way, with no key, so a writer that computes it needs no validator.
/// `container` must be at least 20 bytes.
[[nodiscard]] cc::fixed_array<byte, 16> dxbc_digest(cc::span<byte const> container);
} // namespace sgl::ir
