#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>

namespace sr_test
{
struct backend_entry;
}

// The backends compiled into shaped-rendering-test, each an entry driver that invokes every sg::context_handle test on one context of it.
// Each tests/backends/<backend>-entry.cc registers its driver here at static init, and backends.cc aliases every
// invocable to one run per driver, so `dev.py test "sr - <name>"` runs that test on each backend this binary has.
// The same shape as shaped-graphics-test's; libs/base/nexus/docs/invocable-tests.md is the alias mechanism.

struct sr_test::backend_entry
{
    cc::string driver; ///< the entry driver's test name, e.g. "sr vulkan"
    cc::string invoke; ///< the group it invokes under, e.g. "vulkan"
};

namespace sr_test
{
/// The registered backends; a function-local static, so the order the entry files initialize in does not matter.
cc::vector<backend_entry>& backends();

/// Appends a backend; returns true so it can seed a static-init bool.
inline bool register_backend(cc::string driver, cc::string invoke)
{
    backends().push_back(backend_entry{.driver = cc::move(driver), .invoke = cc::move(invoke)});
    return true;
}
} // namespace sr_test
