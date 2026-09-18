#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh> // sg::context_handle

namespace sg_test
{
struct backend_entry;
struct context_factory;
} // namespace sg_test

// Backends compiled into the sg API test binary (shaped-graphics-test).
// Each backend entry file (tests/backends/<backend>-entry.cc) registers its driver here at static-init.
// The alias setup (tests/backends/backends.cc) reads the table to build, per context_handle invocable, one alias fragment per backend.
// See libs/base/nexus/docs/invocable-tests.md for the alias mechanism.

struct sg_test::backend_entry
{
    cc::string driver; ///< the entry-point driver test name, e.g. "sg dx12 warp backend"
    cc::string invoke; ///< the nx::invoke_tests group it dispatches under, e.g. "dx12-warp"
};

// A way to make a fresh context of one backend, for a test or benchmark that needs its own rather than the driver's.
struct sg_test::context_factory
{
    cc::string backend; ///< e.g. "dx12"
    cc::unique_function<cc::result<sg::context_handle>()> create;
};

namespace sg_test
{

// The registered backends (function-local static: order-independent across TUs). Defined in backends.cc.
cc::vector<backend_entry>& backends();

// Appends a backend; returns true so it can seed a static-init bool.
inline bool register_backend(cc::string driver, cc::string invoke)
{
    backends().push_back(backend_entry{.driver = cc::move(driver), .invoke = cc::move(invoke)});
    return true;
}

// The registered context factories, one per backend compiled in.
// Defined in backends.cc.
cc::vector<context_factory>& context_factories();

// Appends a factory; returns true so it can seed a static-init bool.
inline bool register_context_factory(cc::string backend, cc::unique_function<cc::result<sg::context_handle>()> create)
{
    context_factories().push_back(context_factory{.backend = cc::move(backend), .create = cc::move(create)});
    return true;
}
} // namespace sg_test
