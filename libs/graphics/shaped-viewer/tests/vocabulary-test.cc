#include <clean-core/container/map.hh>
#include <nexus/test.hh>
#include <shaped-viewer/camera.hh>
#include <shaped-viewer/pbr_material.hh>
#include <shaped-viewer/resources/resource_ids.hh>
#include <shaped-viewer/view_id.hh>
#include <shaped-viewer/viewer_definition.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

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
