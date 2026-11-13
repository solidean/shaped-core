#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/fwd.hh>

namespace sgl::ir
{
/// `p` as a DXIL container D3D12 can load, or the construct DXIL has no lowering for yet.
///
/// `p` must have been through `construct_ssa`, since DXIL takes no variable that is a whole vector.
/// Nothing here validates: the container carries the bypass digest, which is how an unsigned shader reaches a pipeline.
[[nodiscard]] cc::optional<cc::string> write_dxil(program const& p, cc::vector<byte>& out);
} // namespace sgl::ir
