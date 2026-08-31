#include <shaped-shader-library/compiler/dxc_compiler.hh>

// sv's material shaders are generated for its DXR path and compiled to DXIL, whose reflection reads a container
// beside the bytecode through the Windows SDK's d3d12shader.h — a header the Linux DXC release does not ship.
// So these need Windows as well as a DXC build, unlike the rest of the viewer suite.
#if SLIB_HAS_DXC && defined(CC_OS_WINDOWS)

#include "viewer_test_env.hh"

#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/resources/material_shader_cache.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// sv::material_shader_cache: one compiled closest-hit per permutation, and the dedup that makes the two-key split pay.
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

[[nodiscard]] sv::mesh make_mesh()
{
    // Resolution reads the lists and the summary, never the geometry itself, so a stand-in id is all this needs.
    return {.name = "tri", .geometry = sv::mesh_id(0), .triangle_count = 1, .vertex_count = 3};
}

/// Drives a permutation's compiles to completion and fails the test with DXC's own message when one did not build.
///
/// BOTH nodes, because a permutation whose material can cut out carries an any-hit as well.
/// A compile left undriven is async work still holding this test's context when it ends, which nexus reports as a failure of
/// the test itself — intermittently, since a node that happens to settle first is never noticed.
void require_compiled(sv::material_permutation const& p)
{
    REQUIRE(p.shader != nullptr);
    (void)cc::try_async_blocking_get(p.shader);
    if (p.shader->has_error())
        FAIL(cc::format("{}\n--- source ---\n{}", p.shader->try_error()->underlying().to_string(), p.source));
    REQUIRE(p.shader->has_value());
    CHECK(p.shader->try_value()->stage == sg::shader_stage::closest_hit);
    CHECK(p.shader->try_value()->bytecode.size() > 0);

    // A material that never writes `geometry_opacity` deliberately has no any-hit: one that could reject nothing would
    // still cost the hardware its opaque path on every intersection.
    if (!p.can_cut_out)
    {
        CHECK(p.any_hit == nullptr);
        CHECK(p.shadow_any_hit == nullptr);
        return;
    }

    // Two of them, because the ray that shades and the ray that shadows carry different payloads and an any-hit declares
    // exactly one — so a permutation that can cut out gets a record and an entry point for each.
    for (auto const* const node : {&p.any_hit, &p.shadow_any_hit})
    {
        REQUIRE(*node != nullptr);
        (void)cc::try_async_blocking_get(*node);
        if ((*node)->has_error())
            FAIL(cc::format("{}\n--- source ---\n{}", (*node)->try_error()->underlying().to_string(), p.source));
        REQUIRE((*node)->has_value());
        CHECK((*node)->try_value()->stage == sg::shader_stage::any_hit);
        CHECK((*node)->try_value()->bytecode.size() > 0);
    }
}
} // namespace

TEST("sv::material_shader_cache - the fallback stands in for a permutation that did not compile")
{
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the generated shaders");

    auto cache = sv::material_shader_cache::create(
        sg::shader_format::dxil, {.epilogue_include = sv::material_shader_cache::hit_epilogue_include});

    auto const& fallback = cache.acquire_fallback();
    require_compiled(fallback);

    // The whole reason it can stand in for anything: an empty signature reads NO parameter block, so an instance whose
    // block was laid out for some other material is simply never touched.
    CHECK(fallback.layout.slots.empty());
    CHECK(fallback.layout.size_bytes == 0);

    // Nothing to sample and nothing to cut out, so it needs neither a sampler nor an any-hit.
    CHECK(fallback.samplers.empty());
    CHECK(!fallback.can_cut_out);

    // Compiled once per cache, like any other permutation.
    CHECK(&cache.acquire_fallback() == &fallback);
    auto const count = cache.count();
    CHECK(&cache.acquire_fallback() == &fallback);
    CHECK(cache.count() == count);

    // The case it exists for: a material type whose fragment does not compile.
    // Its permutation comes back with an ERROR rather than a shader, which is what the trace substitutes on — and
    // before this it made the whole view a no-op instead of just its own meshes.
    auto lib = sv::material_library::create();
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("roughness", 0.5f));
    auto const broken = lib.register_type(
        sv::material_type::create("broken", cc::move(signature), "    surface.nonsense = not_a_function(roughness);"));
    auto const id = lib.acquire(sv::material::create("broken", broken, {}));

    auto const& bad = cache.acquire(sv::resolve_material(lib, id, make_mesh()));
    (void)cc::try_async_blocking_get(bad.shader);
    CHECK(bad.shader->has_error());
    CHECK(bad.shader->try_value() == nullptr);

    // ...and it is a different permutation from the fallback, so substituting is a choice the trace makes rather than
    // something the cache did behind it.
    CHECK(&bad != &fallback);
}

TEST("sv::material_shader_cache - two materials of one permutation are one compile")
{
    if (!sv_test::shared_env().has_compiler)
        return; // no DXC installed: nothing here can compile

    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();

    auto gold_b = cc::vector<sv::material_attribute_binding>();
    gold_b.push_back(sv::material_attribute_binding::of("roughness", 0.2f));
    auto copper_b = cc::vector<sv::material_attribute_binding>();
    copper_b.push_back(sv::material_attribute_binding::of("roughness", 0.6f));

    auto const gold = lib.acquire(sv::material::create("gold", pbr, gold_b));
    auto const copper = lib.acquire(sv::material::create("copper", pbr, copper_b));

    auto cache = sv::material_shader_cache::create(
        sg::shader_format::dxil, {.epilogue_include = sv::material_shader_cache::hit_epilogue_include});
    auto const mesh = make_mesh();

    auto const& first = cache.acquire(sv::resolve_material(lib, gold, mesh));
    require_compiled(first);
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
    require_compiled(sampled);
    CHECK(cache.count() == 2);
    CHECK(&sampled != &first);

    // The layout comes back with the shader, from the same generate — which is what keeps the offsets the CPU fills and the ones
    // the shader reads from being two computations of the same thing.
    CHECK(first.layout.size_bytes > 0);
    CHECK(sampled.layout.slots.size()
          > first.layout.slots.size()); // a sampled attribute takes a texture index AND a uv descriptor
}

TEST("sv::material_shader_cache - every builtin type compiles as a closest-hit")
{
    if (!sv_test::shared_env().has_compiler)
        return;

    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);

    auto cache = sv::material_shader_cache::create(
        sg::shader_format::dxil, {.epilogue_include = sv::material_shader_cache::hit_epilogue_include});
    auto const mesh = make_mesh();

    for (auto const& name : {sv::builtin_material::pbr, sv::builtin_material::unlit})
    {
        auto const type = lib.acquire_type(name).value();
        auto const id = lib.acquire(sv::material::create(cc::string(name), type, {}));
        require_compiled(cache.acquire(sv::resolve_material(lib, id, mesh)));
    }

    CHECK(cache.count() == 2); // two types, two permutations — nothing collapsed that should not have

    // find() answers for what was acquired, and only for that.
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();
    auto const gold = lib.acquire(sv::material::create("gold", pbr, {}));
    auto const resolved = sv::resolve_material(lib, gold, mesh);
    CHECK(cache.find(sv::material_shader_key(resolved.permutation_key, cache.generation_options())) != nullptr);

    // The resolution's shape alone is NOT the key — the options it was generated under are part of it.
    CHECK(cache.find(resolved.permutation_key) == nullptr);
    CHECK(cache.find(cc::hash128{}) == nullptr);
}

#endif // SLIB_HAS_DXC
