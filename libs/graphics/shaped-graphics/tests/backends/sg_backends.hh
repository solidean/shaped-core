#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>

// Backends compiled into the sg API test binary (shaped-graphics-test). Each backend entry file
// (tests/backends/<backend>-entry.cc) registers its driver here at static-init; the alias setup
// (tests/backends/backends.cc) reads the table to build, per context_handle invocable, one alias
// fragment per backend. See libs/base/nexus/docs/invocable-tests.md for the alias mechanism.

namespace sg_test
{
struct backend_entry
{
    cc::string driver; ///< the entry-point driver test name, e.g. "sg dx12 warp backend"
    cc::string invoke; ///< the nx::invoke_tests group it dispatches under, e.g. "dx12-warp"
};

// The registered backends (function-local static: order-independent across TUs). Defined in backends.cc.
cc::vector<backend_entry>& backends();

// Appends a backend; returns true so it can seed a static-init bool.
inline bool register_backend(cc::string driver, cc::string invoke)
{
    backends().push_back(backend_entry{.driver = cc::move(driver), .invoke = cc::move(invoke)});
    return true;
}
} // namespace sg_test
