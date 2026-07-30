#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/camera.hh>
#include <shaped-viewer/gpu_types.hh>
#include <shaped-viewer/light.hh>
#include <shaped-viewer/pbr_material.hh>
#include <shaped-viewer/rendering/frame_constants.hh>
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/resources/resource_ids.hh>
#include <shaped-viewer/view_id.hh>
#include <shaped-viewer/viewer_definition.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

#include <type_traits>

// CPU-only invariants of the viewer's vocabulary — no GPU, so these run in every configuration.

TEST("sv - view_id from_string is stable and distinct")
{
    auto const a = sv::view_id::from_string("main-viewport");
    auto const b = sv::view_id::from_string("main-viewport");
    auto const c = sv::view_id::from_string("secondary");

    CHECK(a == b);
    CHECK(a.value == b.value);
    CHECK(a != c);
    CHECK(a.value != c.value);
}

TEST("sv - view_id keys a map")
{
    auto m = cc::map<sv::view_id, int>();
    m[sv::view_id::from_string("a")] = 1;
    m[sv::view_id::from_string("b")] = 2;

    CHECK(m.get(sv::view_id::from_string("a")) == 1);
    CHECK(m.get(sv::view_id::from_string("b")) == 2);
    CHECK(m.contains(sv::view_id::from_string("b")));
    CHECK(!m.contains(sv::view_id::from_string("c")));
}

TEST("sv - resource ids compare and key a map")
{
    CHECK(sv::mesh_id::invalid == sv::mesh_id(cc::u32(-1)));
    CHECK(sv::mesh_id::invalid != sv::mesh_id(0)); // 0 is a usable id now, not the sentinel
    CHECK(sv::mesh_id(1) != sv::mesh_id(2));

    auto m = cc::map<sv::mesh_id, int>();
    m[sv::mesh_id(1)] = 10;
    m[sv::mesh_id(2)] = 20;
    CHECK(m.get(sv::mesh_id(1)) == 10);
    CHECK(m.get(sv::mesh_id(2)) == 20);
}

TEST("sv - triangle_data hashes content, not identity")
{
    // Spelled out rather than copied: the point is two independent allocations holding equal bytes.
    auto const a = cc::vector<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0)};
    auto const same_content = cc::vector<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0)};
    auto const moved_vertex = cc::vector<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 2, 0)};

    CHECK(a.data() != same_content.data());
    CHECK(sv::triangle_data::create(a).hash == sv::triangle_data::create(same_content).hash);
    CHECK(sv::triangle_data::create(a).hash != sv::triangle_data::create(moved_vertex).hash);
}

TEST("sv - triangle_data pins its positions")
{
    // An owning rvalue moves into the pin, so the data survives the source going away — that is what lets a
    // manager read it on a cache miss long after the acquire call site is gone.
    auto const data = []
    {
        auto positions = cc::vector<tg::pos3f>{tg::pos3f(1, 2, 3), tg::pos3f(4, 5, 6), tg::pos3f(7, 8, 9)};
        return sv::triangle_data::create(cc::move(positions));
    }();

    REQUIRE(data.positions.size() == 3);
    CHECK(data.positions[0] == tg::pos3f(1, 2, 3));
    CHECK(data.positions[2] == tg::pos3f(7, 8, 9));

    // A copy shares the pin rather than duplicating the elements.
    auto const shared = sv::triangle_data(data);
    CHECK(shared.positions.data() == data.positions.data());
    CHECK(shared.hash == data.hash);
}

TEST("sv - indexed_triangle_data hashes both buffers")
{
    auto const positions = cc::vector<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0)};
    auto const indices = cc::vector<cc::u32>{0, 1, 2};
    auto const rewound = cc::vector<cc::u32>{0, 2, 1};

    auto const base = sv::indexed_triangle_data::create(positions, indices);
    CHECK(base.hash == sv::indexed_triangle_data::create(positions, indices).hash);
    // Same positions, different winding — a different mesh, so a different key.
    CHECK(base.hash != sv::indexed_triangle_data::create(positions, rewound).hash);
    // ... and it must not collide with the non-indexed data over the same positions either.
    CHECK(base.hash != sv::triangle_data::create(positions).hash);
}

TEST("sv - material_data hashes content")
{
    auto const red = sv::pbr_material{.base_color = tg::vec3f(1, 0, 0)};
    auto const green = sv::pbr_material{.base_color = tg::vec3f(0, 1, 0)};

    auto const a = cc::vector<sv::pbr_material>{red, green};
    auto const same_content = cc::vector<sv::pbr_material>{red, green}; // different storage, equal bytes
    auto const reordered = cc::vector<sv::pbr_material>{green, red};    // same set, different triangle order

    CHECK(sv::material_data::create(a).hash == sv::material_data::create(same_content).hash);
    CHECK(sv::material_data::create(a).hash != sv::material_data::create(reordered).hash);
}

TEST("sv - pbr_material_gpu::from preserves fields")
{
    auto const m = sv::pbr_material{.base_color = tg::vec3f(0.1f, 0.2f, 0.3f),
                                    .metallic = 0.5f,
                                    .roughness = 0.25f,
                                    .emissive = tg::vec3f(1, 0, 0)};
    auto const g = sv::pbr_material_gpu::from(m);

    CHECK(g.base_color == m.base_color);
    CHECK(g.metallic == m.metallic);
    CHECK(g.roughness == m.roughness);
    CHECK(g.emissive == m.emissive);
}

TEST("sv - area_light_gpu::from lays out the rect and its emitting face")
{
    static_assert(sizeof(sv::area_light_gpu) == 80); // five 16-byte cbuffer lanes

    auto const light = sv::area_light{.center = tg::pos3f(0, 3, 0),
                                      .half_extent_u = tg::vec3f(0.75f, 0, 0),
                                      .half_extent_v = tg::vec3f(0, 0, 0.5f),
                                      .emission = tg::vec3f(12, 12, 12)};
    auto const g = sv::area_light_gpu::from(light);

    CHECK(g.center == tg::vec3f(0, 3, 0));
    CHECK(g.u == light.half_extent_u);
    CHECK(g.v == light.half_extent_v);
    CHECK(g.emission == light.emission);
    CHECK(g.normal == tg::vec3f(0, -1, 0)); // cross(+x, +z) faces down

    SECTION("swapping the two half-extents spans the same rect, flipping the face")
    {
        auto flipped = light;
        flipped.half_extent_u = light.half_extent_v;
        flipped.half_extent_v = light.half_extent_u;
        CHECK(sv::area_light_gpu::from(flipped).normal == tg::vec3f(0, 1, 0));
    }

    SECTION("emission has no usable default")
    {
        // Negative on every channel, so a light that was never given an emission is reported on use instead of
        // tracing as a black rect.
        CHECK(sv::area_light{}.emission[0] < 0);
        CHECK(sv::area_light{}.emission[1] < 0);
        CHECK(sv::area_light{}.emission[2] < 0);
    }
}

TEST("sv - gpu_boolean packs a bool into one 32-bit lane")
{
    static_assert(sizeof(sv::gpu_boolean) == 4);
    static_assert(std::is_trivially_copyable_v<sv::gpu_boolean>); // it rides into a cbuffer by memcpy

    auto const t = sv::gpu_boolean(true);
    auto const f = sv::gpu_boolean();

    CHECK(t.value == 1u);
    CHECK(f.value == 0u);
    CHECK(bool(t));
    CHECK(!bool(f));
    CHECK(t == sv::gpu_boolean(true));
    CHECK(t != f);

    // A shader reads any non-zero lane as `true`, so an off-by-one bit pattern is still equal to `true` here.
    auto raw = sv::gpu_boolean();
    raw.value = 0xFFFFFFFFu;
    CHECK(bool(raw));
    CHECK(raw == t);
}

TEST("sv - gpu_boolean is a drop-in cbuffer field")
{
    static_assert(sizeof(sv::frame_constants_gpu) == 256);

    auto fc = sv::frame_constants_gpu{};
    CHECK(fc.mesh_is_indexed.value == 0u);

    auto const record_is_indexed = true; // what a caller has: a plain bool off the mesh record
    fc.mesh_is_indexed = record_is_indexed;
    CHECK(fc.mesh_is_indexed.value == 1u);
}

TEST("sv - camera aims at the target")
{
    // The default orientation frames the origin from `position`.
    auto cam = sv::camera{.position = tg::pos3d(0, 0, -5)};
    cam.projection.aspect_ratio = 800.0 / 600.0;

    auto const c = sv::camera_gpu::from(cam);
    // Looking from -z toward the origin, forward is +z.
    CHECK(c.forward[2] > 0.99f);
    CHECK(c.position == tg::vec3f(0, 0, -5));

    // look_at reproduces the same aim explicitly.
    cam.look_at(tg::pos3d::zero);
    CHECK(sv::camera_gpu::from(cam).forward[2] > 0.99f);
}

TEST("sv - camera factories aim at the target")
{
    SECTION("looking_at places the eye and aims forward at the target")
    {
        auto const cam = sv::camera::looking_at(tg::pos3d(0, 0, -5), tg::pos3d::zero);
        CHECK(cam.position == tg::pos3d(0, 0, -5));
        CHECK(sv::camera_gpu::from(cam).forward[2] > 0.99f); // from -z toward the origin -> forward +z
    }

    SECTION("orbiting at azimuth=elevation=0 sits at target - distance*z, looking inward")
    {
        auto const cam = sv::camera::orbiting(tg::pos3d(1, 2, 3), 5.0, tg::angle_d::make_from_degree(0),
                                              tg::angle_d::make_from_degree(0));
        CHECK(cam.position == tg::pos3d(1, 2, -2));
        CHECK(sv::camera_gpu::from(cam).forward[2] > 0.99f);
    }

    SECTION("orbit elevation lifts the eye and preserves the distance")
    {
        auto const cam = sv::camera::orbiting(tg::pos3d::zero, 4.0, tg::angle_d::make_from_degree(0),
                                              tg::angle_d::make_from_degree(30));
        CHECK(cam.position[1] > 0.0); // lifted above the horizon
        auto const d = (cam.position - tg::pos3d::zero).length();
        CHECK(d > 3.999);
        CHECK(d < 4.001);
    }
}

TEST("sv - viewer_definition assembles a view")
{
    auto def = sv::viewer_definition{};

    auto v = sv::view{};
    v.id = sv::view_id::from_string("v0");
    v.size = tg::vec2i(640, 480);
    v.items.push_back({.mesh = sv::mesh_id(1), .materials = sv::material_set_id(1)});
    def.views.push_back(cc::move(v));

    REQUIRE(def.views.size() == 1);
    CHECK(def.views[0].items.size() == 1);
    CHECK(def.views[0].size == tg::vec2i(640, 480));
    CHECK(def.views[0].items[0].kind == sv::scene_item_kind::triangle_mesh);
}
