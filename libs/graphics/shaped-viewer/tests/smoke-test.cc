#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

// Confirms the library builds, links, and its umbrella header compiles.
// The feature tests live alongside the features, in the files next to this one.
TEST("sv smoke - links")
{
    CHECK(true);
}

// The shader library is process-wide rather than a viewer's, because a generated material permutation is compiled from the render
// path, which has no viewer to reach back to.
//
// Deliberately order-independent: this binary's GPU tests reach the same library through sv_test::shared_env, and whichever runs first is the one that builds it.
// What is pinned here is that everyone lands on that one library, not who made it.
//
// Acquired through shared_env rather than directly, and that is a requirement rather than a shortcut: `sv::acquire_shader_library`
// is documented as not thread-safe, so two tests racing its memo both build a library and trip slib's one-per-process guard.
// shared_env's function-local static is what serializes every acquisition in this binary into one.
TEST("sv - the shader library is created once and shared")
{
    auto const& env = sv_test::shared_env();
    REQUIRE(env.lib != nullptr);

    CHECK(sv::acquire_shader_library().value() == env.lib);

    // Clearing the hook does not un-cache what it already answered with — a viewer already compiling through it keeps working.
    sv::set_acquire_shader_library({});
    CHECK(sv::acquire_shader_library().value() == env.lib);
}
