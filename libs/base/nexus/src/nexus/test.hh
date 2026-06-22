#pragma once

#include <nexus/tests/check.hh>
#include <nexus/tests/config.hh>
#include <nexus/tests/section.hh>

#include <clean-core/macros.hh>

#include <source_location>

namespace nx::impl
{
void register_test(char const* name, config::cfg test_config, void (*fn)(), std::source_location loc);
}

#define NX_IMPL_TEST(name, unique_id, ...)                                                                                                      \
    static void CC_MACRO_JOIN(_nx_test_fn_, unique_id)();                                                                                       \
    static const bool CC_MACRO_JOIN(_nx_test_reg_, unique_id) = (::nx::impl::register_test(                                                     \
                                                                     name,                                                                      \
                                                                     []()                                                                       \
                                                                     {                                                                          \
                                                                         using namespace nx::config;                                            \
                                                                         return ::nx::impl::merge_config(__VA_ARGS__);                          \
                                                                     }(),                                                                       \
                                                                     &CC_MACRO_JOIN(_nx_test_fn_, unique_id), std::source_location::current()), \
                                                                 true);                                                                         \
    static void CC_MACRO_JOIN(_nx_test_fn_, unique_id)()

#define TEST(name, ...) NX_IMPL_TEST(name, __COUNTER__, __VA_ARGS__)
