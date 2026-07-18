#include "fake_routines.hh"

#include <nexus/test.hh>
#include <shaped-rendering/render_routine_library.hh>
#include <shaped-shader-library/shader_library.hh>

// Framework orchestration tests that need no GPU context: singleton acquire, the dependency-closure
// walk, dedup, cycle detection, and the global shader-reload generation. They run on every platform.
// Where a deterministic routine count matters, a fresh instance is built with sr::make_package rather
// than the shared singleton.

TEST("sr - a package singleton acquire returns the same instance")
{
    CHECK(sr_test::leaf_package::acquire() == sr_test::leaf_package::acquire());
}

TEST("sr - add_package flattens a package's routines into the library")
{
    auto const pkg = sr::make_package<sr_test::leaf_package>();

    sr::render_routine_library lib;
    lib.add_package(pkg);

    REQUIRE(lib.routines().size() == 1);
    CHECK(lib.routines()[0] == pkg->routine.shared());
    CHECK(lib.packages().size() == 1);
}

TEST("sr - add_package deduplicates a package by identity")
{
    auto const pkg = sr::make_package<sr_test::leaf_package>();

    sr::render_routine_library lib;
    lib.add_package(pkg);
    lib.add_package(pkg); // same instance again — walked once

    CHECK(lib.packages().size() == 1);
    CHECK(lib.routines().size() == 1);
}

TEST("sr - add_package pulls in the transitive dependency closure")
{
    auto const dep = sr::make_package<sr_test::dependent_package>();

    sr::render_routine_library lib;
    lib.add_package(dep);

    REQUIRE(dep->leaf != nullptr);
    CHECK(lib.packages().size() == 2); // dependent + leaf
    CHECK(lib.routines().size() == 2);
}

TEST("sr - a diamond dependency resolves to a single shared instance")
{
    auto const a = sr::make_package<sr_test::diamond_a>();

    sr::render_routine_library lib;
    lib.add_package(a);

    REQUIRE(a->b != nullptr);
    REQUIRE(a->c != nullptr);
    CHECK(a->b->d == a->c->d); // b and c share one d (the singleton)
    CHECK(a->b->d != nullptr);
    CHECK(lib.packages().size() == 4); // a, b, c, d — each once
    CHECK(lib.routines().size() == 4);
}

TEST("sr - a dependency cycle asserts")
{
    CHECK_ASSERTS(sr::make_package<sr_test::cycle_a>());
}

TEST("sr - a singleton package is shared across libraries")
{
    auto const leaf = sr_test::leaf_package::acquire();

    sr::render_routine_library lib_a;
    sr::render_routine_library lib_b;
    lib_a.add_package(leaf);
    lib_b.add_package(leaf);

    REQUIRE(lib_a.routines().size() == 1);
    REQUIRE(lib_b.routines().size() == 1);
    CHECK(lib_a.routines()[0] == lib_b.routines()[0]); // the same routine instance
}

TEST("sr - a shader reload advances the global generation")
{
    auto const shader_lib = std::make_shared<slib::shader_library>();

    auto const before = slib::current_reload_generation();
    shader_lib->note_reload();
    CHECK(slib::current_reload_generation() == before + 1);
    CHECK(shader_lib->generation() == slib::current_reload_generation());
}
