#pragma once

#include <shaped-shader-compiler-dxc/compiler.hh> // include_resolver
#include <shaped-shader-compiler-dxc/impl/dxc_common.hh>

namespace ssc::dxc::impl
{
/// IDxcIncludeHandler that resolves `#include`s via the caller's resolver (used during preprocess).
[[nodiscard]] ComPtr<IDxcIncludeHandler> make_include_handler(IDxcUtils* utils, include_resolver resolve_include);

/// IDxcIncludeHandler that fails on ANY include — enforces already-flattened source during compile.
[[nodiscard]] ComPtr<IDxcIncludeHandler> make_reject_include_handler();
} // namespace ssc::dxc::impl
