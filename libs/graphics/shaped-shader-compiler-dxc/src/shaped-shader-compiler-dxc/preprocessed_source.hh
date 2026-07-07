#pragma once

#include <clean-core/string/string.hh>

namespace ssc::dxc
{
/// Result of the preprocess pass: HLSL with all macros expanded and `#include`s inlined (DXC emits
/// `#line` directives so later diagnostics still point at the original files). Feed `source` back
/// into compiler::compile via shader_description::source.
struct preprocessed_source
{
    cc::string source;
    cc::string warnings; ///< any diagnostics DXC emitted while preprocessing (empty if none)
};
} // namespace ssc::dxc
