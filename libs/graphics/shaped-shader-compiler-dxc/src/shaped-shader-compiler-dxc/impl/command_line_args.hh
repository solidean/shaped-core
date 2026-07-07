#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-shader-compiler-dxc/compile_options.hh>
#include <shaped-shader-compiler-dxc/shader_description.hh>

#include <string>
#include <vector>

namespace ssc::dxc::impl
{
/// DXC argv is a list of wide strings. We hand the pointers to Compile() at the call site (after the
/// storage is stationary) to avoid dangling on move.
using arg_storage = std::vector<std::wstring>;

/// Full compile argv: `-E <entry> -T <prefix>_<model>` + options. Fails if the stage has no profile
/// prefix yet.
[[nodiscard]] cc::result<arg_storage> build_compile_args(shader_description const& desc, compile_options const& opts);

/// Preprocess argv: `-P -T <prefix>_<model>` + defines/extra (no codegen). Fails as above.
[[nodiscard]] cc::result<arg_storage> build_preprocess_args(shader_description const& desc, compile_options const& opts);

/// Space-joined UTF-8 rendering of the args, for compiler_info::signature (cache/provenance).
[[nodiscard]] cc::string join_args(arg_storage const& args);
} // namespace ssc::dxc::impl
