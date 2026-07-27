#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/pbr_material.hh>
#include <shaped-viewer/rendering/shaders.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

// Shared setup for shaped-viewer's GPU tests (Windows + DXC only).
//
// The generated shader symbols are process-wide globals, and a package may be registered only once per
// process — so the shader library is created exactly once for the whole test binary, here, and every test
// that traces goes through it.

namespace sv_test
{
struct env
{
    slib::shader_library* lib = nullptr; // intentionally leaked: process-wide, lives for the test binary
    bool has_compiler = false;
};

/// The one shader library for this test binary, with sv's and sr's packages registered. `has_compiler` is
/// false when DXC is not installed — a caller SKIPs, since nothing will compile.
///
/// sr's package carries the blit shader (sr::blit_routine), which the view_renderer drives — so both packages
/// must be registered, or acquiring the blit shader faults.
inline env const& shared_env()
{
    static env const e = []
    {
        auto* const lib = new slib::shader_library();
        auto compiler = slib::create_dxc_compiler();
        auto const has_compiler = compiler.has_value();
        if (has_compiler)
            lib->add_compiler(cc::move(compiler.value()));
        lib->add_package(sv::shader_package());
        lib->add_package(sr::shader_package());
        return env{.lib = lib, .has_compiler = has_compiler};
    }();
    return e;
}

/// A tiny deterministic LCG, so a cloud is reproducible across runs (headless test) yet varied.
struct rng
{
    cc::u64 state;
    explicit rng(cc::u64 seed) : state(seed != 0 ? seed : 1) {}

    float unit()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return float((state >> 40) & 0xFFFFFFu) / float(0xFFFFFFu);
    }
    float range(float a, float b) { return a + (b - a) * unit(); }
};

struct triangle_cloud
{
    cc::vector<tg::pos3f> positions;        // non-indexed triangle list (3 per triangle)
    cc::vector<sv::pbr_material> materials; // one per triangle
};

/// A random cloud of `triangle_count` small triangles scattered in a box, each with its own PBR material.
inline triangle_cloud make_triangle_cloud(int triangle_count, cc::u64 seed = 0x5EED1234u)
{
    auto out = triangle_cloud{};
    auto r = rng(seed);

    for (auto i = 0; i < triangle_count; ++i)
    {
        auto const center = tg::pos3f(r.range(-1.6f, 1.6f), r.range(-1.6f, 1.6f), r.range(-1.6f, 1.6f));
        auto const spread = 0.35f;
        for (auto k = 0; k < 3; ++k)
            out.positions.push_back(tg::pos3f(center[0] + r.range(-spread, spread), center[1] + r.range(-spread, spread),
                                              center[2] + r.range(-spread, spread)));

        out.materials.push_back({.base_color = tg::vec3f(r.range(0.05f, 1.0f), r.range(0.05f, 1.0f), r.range(0.05f, 1.0f)),
                                 .metallic = r.range(0.0f, 1.0f),
                                 .roughness = r.range(0.08f, 1.0f),
                                 .emissive = tg::vec3f(0, 0, 0)});
    }
    return out;
}

/// The rectangular ceiling light of a Cornell box. The geometry (a quad in `positions`) emits, and these
/// fields let the caller fill the path tracer's pt_frame_constants_gpu so its next-event estimation samples the
/// exact same rectangle.
struct area_light
{
    tg::vec3f center;   // rect center in world space (on the ceiling plane)
    float half_x;       // half-extent along world x
    float half_z;       // half-extent along world z
    tg::vec3f emission; // emitted radiance (matches the emissive of the light material)
};

/// A Cornell box as a non-indexed triangle list with per-triangle materials, plus its light description.
struct cornell_box
{
    cc::vector<tg::pos3f> positions;        // non-indexed triangle list (3 per triangle)
    cc::vector<sv::pbr_material> materials; // one per triangle
    area_light light;
};

/// Appends a quad (a-b-c + a-c-d) carrying material `m` to a Cornell box. Winding is irrelevant — the
/// closest-hit shades two-sided.
inline void cb_push_quad(cornell_box& cb, tg::pos3f a, tg::pos3f b, tg::pos3f c, tg::pos3f d, sv::pbr_material const& m)
{
    cb.positions.push_back(a);
    cb.positions.push_back(b);
    cb.positions.push_back(c);
    cb.positions.push_back(a);
    cb.positions.push_back(c);
    cb.positions.push_back(d);
    cb.materials.push_back(m);
    cb.materials.push_back(m);
}

/// Appends an axis-aligned box spanning [lo, hi] (six quads) carrying material `m`.
inline void cb_push_box(cornell_box& cb, tg::pos3f lo, tg::pos3f hi, sv::pbr_material const& m)
{
    auto const x0 = lo[0];
    auto const y0 = lo[1];
    auto const z0 = lo[2];
    auto const x1 = hi[0];
    auto const y1 = hi[1];
    auto const z1 = hi[2];

    cb_push_quad(cb, tg::pos3f(x0, y0, z0), tg::pos3f(x1, y0, z0), tg::pos3f(x1, y0, z1), tg::pos3f(x0, y0, z1),
                 m); // bottom
    cb_push_quad(cb, tg::pos3f(x0, y1, z0), tg::pos3f(x0, y1, z1), tg::pos3f(x1, y1, z1), tg::pos3f(x1, y1, z0), m); // top
    cb_push_quad(cb, tg::pos3f(x0, y0, z0), tg::pos3f(x0, y1, z0), tg::pos3f(x1, y1, z0), tg::pos3f(x1, y0, z0), m); // front
    cb_push_quad(cb, tg::pos3f(x0, y0, z1), tg::pos3f(x1, y0, z1), tg::pos3f(x1, y1, z1), tg::pos3f(x0, y1, z1), m); // back
    cb_push_quad(cb, tg::pos3f(x0, y0, z0), tg::pos3f(x0, y0, z1), tg::pos3f(x0, y1, z1), tg::pos3f(x0, y1, z0), m); // left
    cb_push_quad(cb, tg::pos3f(x1, y0, z0), tg::pos3f(x1, y1, z0), tg::pos3f(x1, y1, z1), tg::pos3f(x1, y0, z1), m); // right
}

/// A classic Cornell box in a unit-ish cube centered at the origin, open toward -z (the camera side): white
/// floor / ceiling / back, a red left wall and a green right wall, two white boxes on the floor, and a
/// rectangular emitter just below the ceiling. All geometry is already in world space (identity transform).
inline cornell_box make_cornell_box()
{
    auto const white = sv::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f),
                                        .metallic = 0.0f,
                                        .roughness = 1.0f,
                                        .emissive = tg::vec3f(0, 0, 0)};
    auto const red = sv::pbr_material{.base_color = tg::vec3f(0.63f, 0.06f, 0.05f),
                                      .metallic = 0.0f,
                                      .roughness = 1.0f,
                                      .emissive = tg::vec3f(0, 0, 0)};
    auto const green = sv::pbr_material{.base_color = tg::vec3f(0.12f, 0.45f, 0.09f),
                                        .metallic = 0.0f,
                                        .roughness = 1.0f,
                                        .emissive = tg::vec3f(0, 0, 0)};

    auto const light_emission = tg::vec3f(15, 15, 15);
    // A pure emitter: no reflection (albedo 0), so a path that lands on it terminates on its emission.
    auto const lamp = sv::pbr_material{.base_color = tg::vec3f(0, 0, 0),
                                       .metallic = 0.0f,
                                       .roughness = 1.0f,
                                       .emissive = light_emission};

    auto cb = cornell_box{};

    // enclosure: floor, ceiling, back wall, colored side walls (no front wall — the box opens toward the camera)
    cb_push_quad(cb, tg::pos3f(-1, -1, -1), tg::pos3f(1, -1, -1), tg::pos3f(1, -1, 1), tg::pos3f(-1, -1, 1), white); // floor
    cb_push_quad(cb, tg::pos3f(-1, 1, -1), tg::pos3f(-1, 1, 1), tg::pos3f(1, 1, 1), tg::pos3f(1, 1, -1), white); // ceiling
    cb_push_quad(cb, tg::pos3f(-1, -1, 1), tg::pos3f(1, -1, 1), tg::pos3f(1, 1, 1), tg::pos3f(-1, 1, 1), white); // back
    cb_push_quad(cb, tg::pos3f(-1, -1, -1), tg::pos3f(-1, -1, 1), tg::pos3f(-1, 1, 1), tg::pos3f(-1, 1, -1),
                 red); // left wall
    cb_push_quad(cb, tg::pos3f(1, -1, -1), tg::pos3f(1, 1, -1), tg::pos3f(1, 1, 1), tg::pos3f(1, -1, 1),
                 green); // right wall

    // two white blocks standing on the floor
    cb_push_box(cb, tg::pos3f(-0.55f, -1.0f, -0.15f), tg::pos3f(-0.05f, -0.35f, 0.4f), white); // short box
    cb_push_box(cb, tg::pos3f(0.1f, -1.0f, 0.05f), tg::pos3f(0.6f, 0.25f, 0.6f), white);       // tall box

    // ceiling light: a rectangle just below the ceiling, emitting straight down
    auto const light_y = 0.998f;
    auto const half_x = 0.35f;
    auto const half_z = 0.35f;
    cb_push_quad(cb, tg::pos3f(-half_x, light_y, -half_z), tg::pos3f(half_x, light_y, -half_z),
                 tg::pos3f(half_x, light_y, half_z), tg::pos3f(-half_x, light_y, half_z), lamp);
    cb.light
        = area_light{.center = tg::vec3f(0, light_y, 0), .half_x = half_x, .half_z = half_z, .emission = light_emission};

    return cb;
}
} // namespace sv_test
