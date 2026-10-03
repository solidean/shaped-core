#include <shaped-shader-library/compiler/dxc_compiler.hh>

// The hit groups compile through the shader library for the context's own format, which these tests reach through dx12's DXC.
// So they need Windows as well as a DXC build, unlike the rest of the viewer suite.
#if SLIB_HAS_DXC && defined(CC_OS_WINDOWS)

#include "viewer_test_env.hh"

#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/resources/material_shader_cache.hh>
#include <shaped-viewer/scene/resident_mesh.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// sv::material_shader_cache: one compiled hit group per permutation, and the dedup that makes the two-key split pay.
// This is where "gold and copper are one shader" stops being a property of the keys and becomes a property of the compiles.

namespace
{
/// A CPU attribute as the binding a GPU mesh carries.
/// The id is arbitrary: resolution matches on name, format and frequency, and never reaches for the buffer behind one.
[[nodiscard]] sv::mesh_attribute_binding bind(sv::mesh_attribute const& a)
{
    auto const per_instance = a.frequency == sv::attribute_frequency::per_instance;
    return sv::mesh_attribute_binding::of(a, per_instance ? sv::attribute_id::invalid : sv::attribute_id(0));
}

[[nodiscard]] sv::resident_mesh make_mesh()
{
    // Resolution reads the lists and the summary, never the geometry itself, so a stand-in id is all this needs.
    return {.name = "tri", .geometry = sv::mesh_id(0), .triangle_count = 1, .vertex_count = 3};
}

/// Drives a permutation's hit group to completion and fails the test with the compiler's own message when it did not build.
///
/// Driven rather than left cold, because a compile left undriven is async work still holding this test's context when it ends.
/// The node is a parameter by pointer, as a coroutine's must be.
cc::shared_async<cc::unit> require_compiled(sg::context* ctx, sv::material_permutation const* p)
{
    REQUIRE(p->hit_group.is_valid());
    (void)ctx->backlog.start(p->hit_group);
    co_await cc::async_settled(p->hit_group);
    if (auto const* const e = p->hit_group->try_error(); e != nullptr)
        FAIL(cc::format("{}\n--- source ---\n{}", e->underlying().to_string(), p->source));

    // One hit shader per ray type of the tracer's ray set: the surface ray's and the shadow ray's.
    REQUIRE(p->hit_group->try_value() != nullptr);
    CHECK(p->hit_group->try_value()->size() == 2);
    co_return;
}

/// Whether nothing here can compile: no DXC to turn the generated SGL's HLSL into DXIL.
[[nodiscard]] bool cannot_compile()
{
    return !sv_test::shared_env().has_compiler;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv::material_shader_cache - the fallback stands in for a permutation that did not compile",
                     (sg::context_handle const& ctx_h))
{
    if (cannot_compile())
        SKIP("no DXC compiler to build the generated hit groups");

    auto cache = sv::material_shader_cache::create(ctx_h.get());

    auto const& fallback = cache.acquire_fallback();
    (void)co_await require_compiled(ctx_h.get(), &fallback);

    // The whole reason it can stand in for anything: an empty signature reads NO parameter block, so an instance whose
    // block was laid out for some other material is simply never touched.
    CHECK(fallback.layout.slots.empty());
    CHECK(fallback.layout.size_bytes == 0);
    CHECK(!fallback.can_cut_out);
    CHECK(fallback.kind == sv::geometry_kind::triangles);

    // Compiled once per cache, like any other permutation.
    CHECK(&cache.acquire_fallback() == &fallback);
    auto const count = cache.count();
    CHECK(&cache.acquire_fallback() == &fallback);
    CHECK(cache.count() == count);

    // One per geometry kind: a substitution has to keep the hit group's kind.
    auto const& quadric_fallback = cache.acquire_quadric_fallback();
    CHECK(&quadric_fallback != &fallback);
    CHECK(quadric_fallback.kind == sv::geometry_kind::quadrics);
    (void)co_await require_compiled(ctx_h.get(), &quadric_fallback);

    // The case it exists for: a material type whose fragment does not compile.
    // Its permutation comes back with an ERROR rather than a hit group, which is what the trace substitutes on.
    auto lib = sv::material_library::create();
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("roughness", 0.5f));
    auto const broken = lib.register_type(
        sv::material_type::create("broken", cc::move(signature), "surface.nonsense = not_a_function(roughness)\n"));
    auto const id = lib.acquire(sv::material::create("broken", broken, {}));

    auto const& bad = cache.acquire(sv::resolve_material(lib, id, make_mesh()));
    (void)ctx_h->backlog.start(bad.hit_group);
    co_await cc::async_settled(bad.hit_group);
    CHECK(bad.hit_group->has_error());
    CHECK(bad.hit_group->try_value() == nullptr);

    // ...and it is a different permutation from the fallback, so substituting is a choice the trace makes rather than
    // something the cache did behind it.
    CHECK(&bad != &fallback);
    co_await cc::async_settled(sv::background_work(*ctx_h));
}

ASYNC_INVOCABLE_TEST("sv::material_shader_cache - two materials of one permutation are one compile",
                     (sg::context_handle const& ctx_h))
{
    if (cannot_compile())
        SKIP("no DXC compiler to build the generated hit groups");

    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();

    auto gold_b = cc::vector<sv::material_attribute_binding>();
    gold_b.push_back(sv::material_attribute_binding::of("roughness", 0.2f));
    auto copper_b = cc::vector<sv::material_attribute_binding>();
    copper_b.push_back(sv::material_attribute_binding::of("roughness", 0.6f));

    auto const gold = lib.acquire(sv::material::create("gold", pbr, gold_b));
    auto const copper = lib.acquire(sv::material::create("copper", pbr, copper_b));

    auto cache = sv::material_shader_cache::create(ctx_h.get());
    auto const mesh = make_mesh();

    auto const& first = cache.acquire(sv::resolve_material(lib, gold, mesh));
    (void)co_await require_compiled(ctx_h.get(), &first);
    CHECK(cache.count() == 1);

    // Differing only in a constant, copper resolves to the same permutation — so it is the SAME compile, not an equal one.
    auto const& second = cache.acquire(sv::resolve_material(lib, copper, mesh));
    CHECK(&second == &first);
    CHECK(cache.count() == 1);

    // A texture is the one thing that changes the generated text, so it is the one thing that costs a second compile.
    auto textured = make_mesh();
    textured.attributes.push_back(
        bind(sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex,
                                        cc::array<tg::vec2f>{tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1)})));
    textured.textures.push_back({.name = "base_color", .source = {.texture = sv::texture_id(1), .uv_attribute = "uv"}});

    auto const& sampled = cache.acquire(sv::resolve_material(lib, gold, textured));
    (void)co_await require_compiled(ctx_h.get(), &sampled);
    CHECK(cache.count() == 2);
    CHECK(&sampled != &first);

    // The layout comes back with the hit group, from the same generate — which is what keeps the offsets the CPU fills and
    // the ones the shader reads from being two computations of the same thing.
    CHECK(first.layout.size_bytes > 0);
    CHECK(sampled.layout.slots.size()
          > first.layout.slots.size()); // a sampled attribute takes a texture index AND a uv descriptor
    co_await cc::async_settled(sv::background_work(*ctx_h));
}

ASYNC_INVOCABLE_TEST("sv::material_shader_cache - every builtin type compiles as a hit group",
                     (sg::context_handle const& ctx_h))
{
    if (cannot_compile())
        SKIP("no DXC compiler to build the generated hit groups");

    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);

    auto cache = sv::material_shader_cache::create(ctx_h.get());
    auto const mesh = make_mesh();

    for (auto const& name : {sv::builtin_material::openpbr, sv::builtin_material::pbr, sv::builtin_material::unlit})
    {
        auto const type = lib.acquire_type(name).value();
        auto const id = lib.acquire(sv::material::create(cc::string(name), type, {}));
        (void)co_await require_compiled(ctx_h.get(), &cache.acquire(sv::resolve_material(lib, id, mesh)));
    }

    CHECK(cache.count() == 3); // three types, three permutations — nothing collapsed that should not have

    // find() answers for what was acquired, and only for that.
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();
    auto const gold = lib.acquire(sv::material::create("gold", pbr, {}));
    auto const resolved = sv::resolve_material(lib, gold, mesh);
    CHECK(cache.find(sv::material_shader_key(resolved.permutation_key, {})) != nullptr);

    // The resolution's shape alone is NOT the key — the geometry it was generated for is part of it.
    CHECK(cache.find(resolved.permutation_key) == nullptr);
    CHECK(cache.find(cc::hash128{}) == nullptr);
    co_await cc::async_settled(sv::background_work(*ctx_h));
}

#endif // SLIB_HAS_DXC
