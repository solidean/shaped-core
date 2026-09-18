#include <clean-core/common/hash.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/rendering/frame_constants.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh> // pt_light_table, pt_frame_constants_gpu
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/scene/background.hh>
#include <shaped-viewer/scene/light.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <shaped-viewer/scene/pbr_material.hh>
#include <shaped-viewer/scene/triangle_geometry.hh>
#include <shaped-viewer/stable_id.hh>
#include <shaped-viewer/view/camera.hh>
#include <shaped-viewer/view/layer.hh>
#include <shaped-viewer/view/viewer_definition.hh>
#include <typed-geometry/geometry/primitives/triangle.hh>
#include <typed-geometry/linalg/cross.hh> // tg::cross + tg::dual
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/pos_ops.hh> // tg::distance
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::normalize
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

#include <type_traits>


using namespace cc::primitive_defines;

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

TEST("sv - the id stack separates views that share a name")
{
    auto const bare = sv::view_id::from_string("view");

    // The default seed is the unseeded hash, so pushing nothing changes nothing.
    CHECK(sv::view_id::from_string("view", 0) == bare);

    // Same name, two scopes: the whole point of scoped_id in a loop.
    auto const in_0 = sv::view_id::from_string("view", sv::push_id_seed(0, i64(0)));
    auto const in_1 = sv::view_id::from_string("view", sv::push_id_seed(0, i64(1)));
    CHECK(in_0 != in_1);
    CHECK(in_0 != bare);

    // Seeding is deterministic, or a view would lose its state every run.
    CHECK(in_0 == sv::view_id::from_string("view", sv::push_id_seed(0, i64(0))));

    // Nesting composes, and the order of the path matters.
    auto const outer = sv::push_id_seed(0, cc::string_view("panel"));
    auto const inner = sv::push_id_seed(outer, i64(2));
    CHECK(sv::view_id::from_string("view", inner) != sv::view_id::from_string("view", outer));
    CHECK(sv::push_id_seed(sv::push_id_seed(0, cc::string_view("a")), cc::string_view("b"))
          != sv::push_id_seed(sv::push_id_seed(0, cc::string_view("b")), cc::string_view("a")));
}

TEST("sv - a ## suffix separates ids that share a display name")
{
    // The whole string is hashed, so the suffix is what makes these two views rather than one.
    CHECK(sv::view_id::from_string("angle##0") != sv::view_id::from_string("angle##1"));
    CHECK(sv::view_id::from_string("angle##0") != sv::view_id::from_string("angle"));

    // What a human reads is the part in front of it.
    CHECK(sv::display_name_of("angle##0") == "angle");
    CHECK(sv::display_name_of("angle") == "angle");
    CHECK(sv::display_name_of("") == "");

    // Only the FIRST marker splits, and an id that is nothing but a suffix has no display name at all.
    CHECK(sv::display_name_of("a##b##c") == "a");
    CHECK(sv::display_name_of("##7") == "");

    // A single '#' is not a marker.
    CHECK(sv::display_name_of("a#b") == "a#b");
}

TEST("sv - a formatted id is the string it spells out")
{
    // What `add_view("angle##{}", i)` hashes must be exactly what the pre-formatted string would.
    auto const short_id = cc::format("angle##{}", 2);
    CHECK(short_id == "angle##2");
    CHECK(sv::view_id::from_string(short_id) == sv::view_id::from_string("angle##2"));

    // A long id is spelled out in full rather than truncated.
    auto const padding = cc::string::create_filled(400, 'x');
    auto const long_id = cc::format("{}", padding);
    CHECK(long_id.size() == 400);
    CHECK(long_id == padding);
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
    CHECK(sv::mesh_id::invalid == sv::mesh_id(u32(-1)));
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
    auto const indices = cc::vector<u32>{0, 1, 2};
    auto const rewound = cc::vector<u32>{0, 2, 1};

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

namespace
{
/// The radiance `shaders/background.hlsli` reconstructs along `d`, evaluated on the CPU so the factories can be
/// checked against the basis they are written for rather than against their own coefficients.
/// `d` must be unit.
/// The shader's `max(L, 0)` clamp is deliberately left out — a factory's promise is the unclamped function, and clamping would hide a sign error.
[[nodiscard]] tg::vec3f radiance_along(sv::background const& bg, tg::vec3f d)
{
    auto const x = d[0];
    auto const y = d[1];
    auto const z = d[2];

    f32 const basis[16] = {0.282095f,
                           0.488603f * y,
                           0.488603f * z,
                           0.488603f * x,
                           1.092548f * x * y,
                           1.092548f * y * z,
                           0.315392f * (3 * z * z - 1),
                           1.092548f * x * z,
                           0.546274f * (x * x - y * y),
                           0.590044f * y * (3 * x * x - y * y),
                           2.890611f * x * y * z,
                           0.457046f * y * (5 * z * z - 1),
                           0.373176f * z * (5 * z * z - 3),
                           0.457046f * x * (5 * z * z - 1),
                           1.445306f * z * (x * x - y * y),
                           0.590044f * x * (x * x - 3 * y * y)};

    auto l = tg::vec3f(0, 0, 0);
    for (auto i = 0; i < sv::background::sh_coefficient_count; ++i)
        l = l + bg.sh[i] * basis[i];
    return l;
}

[[nodiscard]] bool near(tg::vec3f a, tg::vec3f b, f32 eps = 1e-4f)
{
    for (auto i = 0; i < 3; ++i)
        if (a[i] - b[i] > eps || b[i] - a[i] > eps)
            return false;
    return true;
}
} // namespace

TEST("sv - background factories reconstruct the radiance they promise")
{
    auto const up = tg::vec3f(0, 1, 0);
    auto const down = tg::vec3f(0, -1, 0);
    auto const side = tg::vec3f(1, 0, 0);

    SECTION("uniform is the same radiance in every direction")
    {
        auto const c = tg::vec3f(0.25f, 0.5f, 0.75f);
        auto const bg = sv::background::uniform(c);

        CHECK(near(radiance_along(bg, up), c));
        CHECK(near(radiance_along(bg, down), c));
        CHECK(near(radiance_along(bg, tg::normalize(tg::vec3f(1, 2, -3))), c));
    }

    SECTION("gradient hits zenith and nadir exactly, averaging on the horizon")
    {
        auto const zenith = tg::vec3f(0.2f, 0.6f, 1.0f);
        auto const nadir = tg::vec3f(0.4f, 0.2f, 0.1f);
        auto const bg = sv::background::gradient(zenith, nadir);

        CHECK(near(radiance_along(bg, up), zenith));
        CHECK(near(radiance_along(bg, down), nadir));
        CHECK(near(radiance_along(bg, side), (zenith + nadir) * 0.5f));
        CHECK(near(radiance_along(bg, tg::normalize(tg::vec3f(0, 0, 1))), (zenith + nadir) * 0.5f));
    }

    SECTION("sun peaks at its direction and falls off as the clamped cosine")
    {
        auto const dir = tg::normalize(tg::vec3f(0.3f, 0.9f, -0.2f));
        auto const c = tg::vec3f(1.0f, 0.8f, 0.6f);
        auto const bg = sv::background::lobe(dir, c);

        // Some unit vector across the lobe's axis, to walk away from the peak with.
        auto const across = tg::dual(tg::cross(dir, tg::vec3f(0, 0, 1))).normalized();

        // The peak is exactly the requested radiance — that is what the 16/17 rescale in `sun` buys.
        CHECK(near(radiance_along(bg, dir), c, 1e-3f));

        // Off the axis it is the TRUNCATED clamped cosine, not the clamped cosine: bands 0..2 leave a floor
        // where the clamp would have given zero — 3/34 of the peak across the lobe, 1/17 behind it.
        CHECK(near(radiance_along(bg, across), c * (3.0f / 34.0f), 1e-3f));
        CHECK(near(radiance_along(bg, -dir), c * (1.0f / 17.0f), 1e-3f));

        // Still a lobe: monotonically brighter as the direction swings back toward the axis.
        auto const at_60 = radiance_along(bg, tg::normalize(dir + across * 1.7320508f)); // tan(60 deg)
        CHECK(at_60[0] > radiance_along(bg, across)[0]);
        CHECK(at_60[0] < c[0]);

        // The direction is normalized for the caller, so its length cannot change the lobe.
        CHECK(near(radiance_along(sv::background::lobe(dir * 7.0f, c), dir), c, 1e-3f));
    }

    SECTION("backgrounds superpose, which is what lets the presets compose")
    {
        auto const sky = sv::background::gradient(tg::vec3f(0.2f, 0.6f, 1.0f), tg::vec3f(0.1f, 0.1f, 0.1f));
        auto const sun = sv::background::lobe(up, tg::vec3f(2, 2, 2));
        auto const d = tg::normalize(tg::vec3f(1, 3, -2));

        CHECK(near(radiance_along(sky.combined_with(sun), d), radiance_along(sky, d) + radiance_along(sun, d)));
        CHECK(near(radiance_along(sky.scaled(0.5f), d), radiance_along(sky, d) * 0.5f));
    }

    SECTION("the presets stay non-negative everywhere a ray can look")
    {
        // Not free: a `sun` dips below zero in a ring behind its lobe, so a preset built on one only reads as a sky if its ambient outweighs that dip.
        // The miss clamps, but a clamp hides that rather than fixing it.
        auto worst = 1.0f;
        for (auto const& bg : {sv::daylight().sky, sv::background::studio()})
            for (auto ix = -2; ix <= 2; ++ix)
                for (auto iy = -2; iy <= 2; ++iy)
                    for (auto iz = -2; iz <= 2; ++iz)
                    {
                        if (ix == 0 && iy == 0 && iz == 0)
                            continue;

                        auto const l = radiance_along(bg, tg::normalize(tg::vec3f(f32(ix), f32(iy), f32(iz))));
                        for (auto ch = 0; ch < 3; ++ch)
                            worst = l[ch] < worst ? l[ch] : worst;
                    }
        CHECK(worst >= 0.0f);
    }
}

TEST("sv - geometry holds both triangle layouts behind one type")
{
    auto const positions
        = cc::vector<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0), tg::pos3f(1, 1, 0)};
    auto const indices = cc::vector<u32>{0, 1, 2, 1, 3, 2};

    auto const raw = sv::triangle_geometry::create_from_triangles(
        cc::vector<tg::triangle3f>{tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0))});
    CHECK(!raw.is_indexed());
    CHECK(raw.vertex_count() == 3); // one triangle unpacks to three positions, sharing its storage
    CHECK(raw.triangle_count() == 1);
    CHECK(raw.positions[1] == tg::pos3f(1, 0, 0));

    auto const indexed = sv::triangle_geometry::create_from_indexed_triangles(positions, indices);
    CHECK(indexed.is_indexed());
    CHECK(indexed.vertex_count() == 4);
    // Two triangles over four vertices — the triangle count follows the index buffer, not the positions.
    CHECK(indexed.triangle_count() == 2);

    CHECK(sv::triangle_geometry{}.is_empty());
}

TEST("sv - geometry carries the resource_data content key across unchanged")
{
    auto const positions = cc::vector<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0)};
    auto const indices = cc::vector<u32>{0, 1, 2};
    auto const triangles = cc::vector<tg::triangle3f>{tg::triangle3f(positions[0], positions[1], positions[2])};

    // The manager caches on the hash alone, so authoring through geometry must key identically to the payload types.
    // A triangle list and the loose positions behind it are the same bytes, so they must also be the same key.
    CHECK(sv::triangle_geometry::create_from_triangles(triangles).hash == sv::triangle_data::create(positions).hash);
    CHECK(sv::triangle_geometry::create_from_indexed_triangles(positions, indices).hash
          == sv::indexed_triangle_data::create(positions, indices).hash);

    auto const g = sv::triangle_geometry::create_from_indexed_triangles(positions, indices);
    auto const data = sv::indexed_triangle_data::from(g);
    CHECK(data.hash == g.hash);
    CHECK(data.positions.data() == g.positions.data()); // the bridge shares the pin, it does not copy
}

TEST("sv - a mesh attribute is typed, pinned and content-hashed")
{
    auto const normals = cc::vector<tg::vec3f>{tg::vec3f(0, 0, 1), tg::vec3f(0, 0, 1), tg::vec3f(0, 1, 0)};

    auto const a = sv::mesh_attribute::create("normal", sv::attribute_frequency::per_vertex, normals);
    CHECK(a.name == "normal");
    CHECK(a.format == sv::attribute_format::of_vector(sv::scalar_type::f32, 3)); // deduced from the element type
    CHECK(a.element_count() == 3);
    CHECK(a.elements_as<tg::vec3f>()[2] == tg::vec3f(0, 1, 0));

    // Equal contents under a different name are still the same bytes: the hash is over the payload only.
    CHECK(sv::mesh_attribute::create("n", sv::attribute_frequency::per_corner, normals).hash == a.hash);
    CHECK(sv::mesh_attribute::create("normal", sv::attribute_frequency::per_vertex,
                                     cc::vector<tg::vec3f>{tg::vec3f(1, 0, 0)})
              .hash
          != a.hash);
}

TEST("sv - attribute_format spans scalars, vectors and matrices")
{
    static_assert(sv::attribute_format_of<f32>.is_scalar());
    static_assert(sv::attribute_format_of<tg::vec3f>.is_vector());
    static_assert(sv::attribute_format_of<tg::mat3f>.is_matrix());

    // The point of scalar + dimensionality: a combination nobody enumerated still has a format.
    static_assert(sv::attribute_format_of<tg::vec4<u8>> == sv::attribute_format::of_vector(sv::scalar_type::u8, 4));
    static_assert(sv::attribute_format_of<tg::vec4<u8>>.size_bytes() == 4);

    // tg::mat is <C, R, T>, so a 4x3 matrix has 3 rows in 4 columns.
    static_assert(sv::attribute_format_of<tg::mat<4, 3, f32>>
                  == sv::attribute_format::of_matrix(sv::scalar_type::f32, 3, 4));
    static_assert(sv::attribute_format_of<tg::mat<4, 3, f32>>.size_bytes() == 48);
    static_assert(sv::attribute_format_of<tg::mat<4, 3, f32>>.component_count() == 12);

    // A position is stored as the vector its components form.
    static_assert(sv::attribute_format_of<tg::pos3f> == sv::attribute_format_of<tg::vec3f>);

    CHECK(sv::scalar_type_size(sv::scalar_type::f64) == 8);
    CHECK(sv::attribute_format{}.size_bytes() == 4); // a default format is one f32

    auto const uvs = cc::vector<tg::vec2f>{tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1)};
    auto const a = sv::mesh_attribute::create("uv", sv::attribute_frequency::per_corner, uvs);
    CHECK(a.format.size_bytes() == 8);
    CHECK(a.element_count() == 3);
}

TEST("sv - a per-instance value is a one-element attribute")
{
    auto const scale = sv::mesh_attribute::create_value("scale", 2.5f);
    auto const tint = sv::mesh_attribute::create_value("tint", tg::vec3f(1, 0, 0));
    auto const on = sv::mesh_attribute::create_value("enabled", true);

    // A per-instance value is typed and counted like any other attribute — it just carries exactly one element.
    CHECK(scale.frequency == sv::attribute_frequency::per_instance);
    CHECK(scale.element_count() == 1);
    CHECK(scale.format == sv::attribute_format_of<f32>);
    CHECK(tint.format == sv::attribute_format_of<tg::vec3f>);
    CHECK(on.format == sv::attribute_format::of_scalar(sv::scalar_type::boolean));

    CHECK(scale.value_as<f32>() == 2.5f);
    CHECK(tint.value_as<tg::vec3f>() == tg::vec3f(1, 0, 0));
    CHECK(on.value_as<bool>());
    CHECK(sv::mesh_attribute::create_value("count", i32(-7)).value_as<i32>() == -7);

    // holds<T>() is what a material asks before reading — a same-width different-type read is still a miss.
    CHECK(scale.holds<f32>());
    CHECK(!scale.holds<i32>());
    CHECK(!scale.holds<tg::vec3f>());

    // Matrices need no inline budget here, so a per-instance value may be one.
    CHECK(sv::mesh_attribute::create_value("uv_transform", tg::mat3f::identity).holds<tg::mat3f>());

    // Content-hashed like every other attribute, and the name is not part of the hash.
    CHECK(sv::mesh_attribute::create_value("other", 2.5f).hash == scale.hash);
}

TEST("sv - a mesh carries the data its material draws it with")
{
    auto m = sv::mesh{.name = "quad"};
    m.geometry = sv::triangle_geometry::create_from_triangles(
        cc::vector<tg::triangle3f>{tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0))});
    m.attributes.push_back(
        sv::mesh_attribute::create("normal", sv::attribute_frequency::per_vertex,
                                   cc::vector<tg::vec3f>{tg::vec3f(0, 0, 1), tg::vec3f(0, 0, 1), tg::vec3f(0, 0, 1)}));
    m.attributes.push_back(sv::mesh_attribute::create_value("fade", 0.5f));
    auto const white = cc::vector<byte>::create_filled(4, byte(0xFF));
    m.textures.push_back({.name = "albedo",
                          .source = {.texture = sv::texture_data::create(white, sg::pixel_format::rgba8_unorm, 1, 1),
                                     .uv_attribute = "uv"}});

    // The lists are the material's input, keyed by name; the mesh only holds them.
    REQUIRE(m.attributes.size() == 2);
    CHECK(m.attributes[0].name == "normal");
    CHECK(m.attributes[0].element_count() == 3);
    CHECK(m.attributes[1].name == "fade"); // a per-instance value rides the same list
    CHECK(m.attributes[1].value_as<f32>() == 0.5f);
    REQUIRE(m.textures.size() == 1);
    CHECK(m.textures[0].name == "albedo");

    // A mesh's texture travels as pixels, exactly as its geometry does — nothing here has met a device.
    CHECK(m.textures[0].source.texture.width == 1);

    // An unauthored mesh draws, casts and receives — the empty flag set would draw nothing.
    CHECK(m.is_visible());
    CHECK(m.flags.has(sv::mesh_flag::casts_shadow));
    CHECK(m.material == sv::material_id::invalid); // an unauthored mesh names no material

    // Copying shares the pinned payload rather than duplicating the buffers.
    auto const copy = m;
    CHECK(copy.geometry.positions.data() == m.geometry.positions.data());
}

TEST("sv - frame_constants_gpu takes a plain bool for its gpu_boolean lane")
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

    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("v0");
    v.resolution = tg::vec2i(640, 480);
    sv::ensure_scene_3d(v).items.push_back({.mesh = sv::mesh_id(1), .instance = sv::instance_id(1)});
    def.views.push_back(cc::move(v));

    REQUIRE(def.views.size() == 1);
    CHECK(def.views[0].resolution == tg::vec2i(640, 480));

    auto const* const scene = sv::primary_scene_3d(def.views[0]);
    REQUIRE(scene != nullptr);
    CHECK(scene->items.size() == 1);
    CHECK(scene->items[0].kind == sv::scene_item_kind::triangle_mesh);
    CHECK(scene->items[0].transform == tg::affine_transform3f::identity); // an unplaced item sits at the origin
}

TEST("sv - a view's geometry lives on a layer, not on the view")
{
    auto v = sv::view_data{};
    CHECK(sv::primary_scene_3d(v) == nullptr); // a fresh view has no layers at all

    auto& scene = sv::ensure_scene_3d(v);
    REQUIRE(v.layers.size() == 1);
    CHECK(v.layers[0].kind == sv::layer_kind::scene_3d);

    // A traced layer writes no meaningful alpha, so it must overwrite rather than blend.
    CHECK(v.layers[0].blend == sv::layer_blend::replace);

    // Asking again returns the same layer rather than appending a second one.
    scene.items.push_back({.mesh = sv::mesh_id(1), .instance = sv::instance_id(1)});
    CHECK(sv::ensure_scene_3d(v).items.size() == 1);
    CHECK(v.layers.size() == 1);
}

TEST("sv - a view starts following its layout and can be pinned to a resolution")
{
    auto v = sv::view_data{};

    // The default is "whatever rect I land in", which is what a caller who never mentions resolution wants.
    CHECK(v.resolution_follows_layout);

    v.resolution = tg::vec2i(320, 240);
    v.resolution_follows_layout = false;
    CHECK(v.resolution == tg::vec2i(320, 240));

    // Refresh is a fraction of the loop's rate, not a frequency: 1 is every frame.
    CHECK(v.refresh.rate == 1.0f);
    CHECK(v.temporal_inputs.empty());
}

TEST("sv - a scene item's placement is an affine tg transform")
{
    auto const quarter_turn = tg::angle_f::make_from_degree(90);
    auto const spin = tg::affine_transform3f::make_rotation(tg::quat_f::make_rotation_y(quarter_turn));
    auto const shift = tg::affine_transform3f::make_translation(tg::vec3f(0, 2, 0));

    // compose(a, b) applies b first, so this rotates the mesh about +y and then lifts it.
    auto const item = sv::scene_item{.transform = tg::compose(shift, spin)};

    // +x turns onto -z under a quarter turn about +y, then the lift moves it up.
    auto const placed = item.transform.transform(tg::pos3f(1, 0, 0));
    CHECK(tg::distance(placed, tg::pos3f(0, 2, -1)) < 1e-5f);

    // The renderer packs these two halves, not a mat4 — the linear part carries no translation of its own.
    CHECK(item.transform.translation() == tg::vec3f(0, 2, 0));
}

// ---- stable ids ------------------------------------------------------------------------------------------

TEST("sv - a light id and a view id are different types over one scheme")
{
    static_assert(!std::is_same_v<sv::view_id, sv::light_id>, "a view's id must not key a light's state");

    auto const a = sv::light_id::from_string("key");
    CHECK(a == sv::light_id::from_string("key"));
    CHECK(a != sv::light_id::from_string("fill"));

    // The same name under a pushed scope is a different id — what makes a loop of lights under scoped_id(i) legal.
    auto const scoped = sv::light_id::from_string("key", sv::push_id_seed(0, i64(3)));
    CHECK(scoped != a);
    CHECK(scoped == sv::light_id::from_string("key", sv::push_id_seed(0, i64(3))));
}

// ---- lights ----------------------------------------------------------------------------------------------

TEST("sv - each light factory picks its path and its natural unit")
{
    using namespace tg::literals;

    auto const point = sv::light::point(tg::pos3f(1, 2, 3));
    CHECK(point.path() == sv::light_path::point);
    CHECK(point.emission.unit == sv::light_unit::candela);
    CHECK(point.placement.translation() == tg::vec3f(1, 2, 3));
    CHECK(point.shaping.kind == sv::light_shaping_kind::none);

    // A spot is a point with a cone, not a path of its own.
    auto const spot = sv::light::spot(tg::pos3f(0, 3, 0), tg::vec3f(0, -1, 0), 30_deg_f, 20_deg_f);
    CHECK(spot.path() == sv::light_path::point);
    CHECK(spot.shaping.kind == sv::light_shaping_kind::cone);
    CHECK(spot.shaping.inner == 20_deg_f);
    CHECK(spot.shaping.outer == 30_deg_f);
    CHECK(near(spot.placement.transform(tg::vec3f(0, 0, -1)), tg::vec3f(0, -1, 0))); // points along -Z

    auto const rect = sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(0.75f, 0, 0), tg::vec3f(0, 0, 0.5f));
    CHECK(rect.path() == sv::light_path::area);
    CHECK(rect.shape() == sv::area_shape::rect);
    CHECK(rect.emission.unit == sv::light_unit::nit);
    CHECK(near(tg::vec3f(rect.half_extents()[0], rect.half_extents()[1], 0), tg::vec3f(0.75f, 0.5f, 0)));

    auto const directional = sv::light::directional(tg::vec3f(0, -1, 0));
    CHECK(directional.path() == sv::light_path::distant_point);
    CHECK(directional.emission.unit == sv::light_unit::lux);

    auto const sun = sv::light::sun(tg::vec3f(0, -1, 0));
    CHECK(sun.path() == sv::light_path::distant_disc);
    CHECK(tg::abs(sun.angular_radius().degree() - 0.265f) < 1e-4f);

    // A sun of no size is parallel light, which is a different path rather than a degenerate disc.
    CHECK(sv::light::sun(tg::vec3f(0, -1, 0), tg::angle_f()).path() == sv::light_path::distant_point);
}

TEST("sv - a light's unit, face and cone are checked against its path")
{
    using namespace tg::literals;

    auto point = sv::light::point(tg::pos3f(0, 0, 0));
    auto rect = sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 1));
    auto sun = sv::light::sun(tg::vec3f(0, -1, 0));

    // Every factory builds a valid light.
    CHECK(sv::light_problem(point).empty());
    CHECK(sv::light_problem(rect).empty());
    CHECK(sv::light_problem(sun).empty());

    // A unit the path accepts is fine, and one it cannot mean asserts where it is written.
    CHECK(sv::light_problem(point.lumens(800)).empty());
    CHECK(sv::light_problem(rect.candela(50)).empty());
    CHECK_ASSERTS(point.nits(10));
    CHECK_ASSERTS(point.lux(10));
    CHECK_ASSERTS(sun.candela(10));
    CHECK_ASSERTS(rect.lux(10));

    // Faces are an area light's, and a cone on a distant light would be a barn door.
    CHECK(sv::light_problem(rect.face(sv::light_face::both)).empty());
    CHECK_ASSERTS(point.face(sv::light_face::back));
    CHECK_ASSERTS(sun.cone(0_deg_f, 10_deg_f));
    CHECK_ASSERTS(point.spread(10_deg_f));
    CHECK_ASSERTS(point.cone(20_deg_f, 10_deg_f)); // inner beyond outer

    // A negative color would emit negative light, and a NaN exposure would drop every path that sees the light.
    CHECK_ASSERTS(point.color(tg::vec3f(1, -0.1f, 1)));
    CHECK_ASSERTS(point.exposure(0.0f / 0.0f));
    CHECK_ASSERTS(point.exposure(200));
    CHECK(sv::light_problem(point.exposure(-3)).empty());

    // The emission is a public aggregate, so a direct write skips the setters; the GPU layout is the gate that remains.
    // Candela on a sun would otherwise be read as lux, silently.
    auto bypassed = sv::light::sun(tg::vec3f(0, -1, 0));
    bypassed.emission.unit = sv::light_unit::candela;
    CHECK(!sv::light_problem(bypassed).empty());
    CHECK_ASSERTS(sv::light_gpu::from(bypassed));
}

TEST("sv - two equal lights compare equal, and any difference is seen")
{
    auto const a = sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 1)).nits(12);
    auto const b = sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 1)).nits(12);
    CHECK(a == b);

    auto brighter = a;
    brighter.nits(13);
    CHECK(brighter != a);

    // Same unit, same intensity, different path: the payload's alternative is part of the comparison.
    CHECK(sv::light::point(tg::pos3f(0, 3, 0)) != sv::light::spot(tg::pos3f(0, 3, 0), tg::vec3f(0, -1, 0), tg::angle_f()));
}

TEST("sv - light_gpu::from lays out the rect and its emitting face")
{
    static_assert(sizeof(sv::light_gpu) == 96); // six 16-byte lanes, as sv::light in shaders/light.hlsli

    auto const light = sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(0.75f, 0, 0), tg::vec3f(0, 0, 0.5f)).nits(12);
    auto const g = sv::light_gpu::from(light);

    CHECK(g.path == u32(sv::light_path::area));
    CHECK(near(g.position, tg::vec3f(0, 3, 0)));
    CHECK(tg::abs(g.area - 4.0f * 0.75f * 0.5f) < 1e-5f); // stored, so both estimators read one number
    CHECK(near(g.emission, tg::vec3f(12, 12, 12)));

    // The same rect the old half-extent spelling described, emitting along cross(u, v) = cross(+x, +z) = -y.
    CHECK(near(g.normal, tg::vec3f(0, -1, 0)));
    CHECK(near(tg::normalize(tg::dual(tg::cross(g.u, g.v))), g.normal));
    CHECK(tg::abs(tg::dual(tg::cross(g.u, g.v)).length() - 0.75f * 0.5f) < 1e-5f);
    CHECK(tg::abs(tg::abs(tg::dot(g.u, tg::vec3f(0, 0, 1))) - 0.5f) < 1e-5f);
    CHECK(tg::abs(tg::abs(tg::dot(g.v, tg::vec3f(1, 0, 0))) - 0.75f) < 1e-5f);

    SECTION("the back face emits the other way")
    {
        auto back = light;
        back.face(sv::light_face::back);
        CHECK(near(sv::light_gpu::from(back).normal, tg::vec3f(0, 1, 0)));
    }

    SECTION("every unit converts to the same radiance through the rect's area")
    {
        // A = 4 * 0.75 * 0.5 = 1.5: 12 nits is 18 cd along the normal and pi * 1.5 * 12 lm from one face.
        auto l = light;
        CHECK(near(sv::light_gpu::from(l.candela(18)).emission, tg::vec3f(12, 12, 12)));
        CHECK(near(sv::light_gpu::from(l.lumens(tg::pi<f32> * 1.5f * 12)).emission, tg::vec3f(12, 12, 12)));
    }

    SECTION("exposure is stops on top, and color only tints")
    {
        auto l = light;
        l.exposure(1).color(tg::vec3f(1, 0.5f, 0));
        CHECK(near(sv::light_gpu::from(l).emission, tg::vec3f(24, 12, 0)));
    }

    SECTION("a uniform scale in the placement grows the area a flux is spread over")
    {
        auto const placement = tg::similarity_transform3f::make_uniform_scaling(2.0f);
        auto const scaled = sv::light::rect(placement, tg::vec2f(1, 1));
        CHECK(tg::abs(scaled.area() - 16.0f) < 1e-5f);
    }
}

TEST("sv - the light table groups by path and keeps each run in the order it was given")
{
    // Tagged by path, told apart by position.x so the order can be read back.
    auto const record = [](sv::light_path path, float tag)
    { return sv::light_gpu{.position = tg::vec3f(tag, 0, 0), .path = u32(path)}; };

    auto const given = cc::vector<sv::light_gpu>{
        record(sv::light_path::area, 0), record(sv::light_path::distant_disc, 1), record(sv::light_path::point, 2),
        record(sv::light_path::area, 3), record(sv::light_path::point, 4)};
    auto const table = sv::pt_light_table::grouped(given);

    // point, then area, then distant_point (empty), then distant_disc.
    CHECK(table.path_count[0] == 2);
    CHECK(table.path_count[1] == 2);
    CHECK(table.path_count[2] == 0);
    CHECK(table.path_count[3] == 1);
    CHECK(table.path_offset[0] == 0);
    CHECK(table.path_offset[1] == 2);
    CHECK(table.path_offset[2] == 4);
    CHECK(table.path_offset[3] == 4);

    auto const tags = cc::vector<float>{2, 4, 0, 3, 1};
    REQUIRE(table.records.size() == tags.size());
    for (auto i = isize(0); i < tags.size(); ++i)
        CHECK(table.records[i].position[0] == tags[i]);

    SECTION("the frame block receives the same table")
    {
        auto fc = sv::pt_frame_constants_gpu{};
        table.describe_in(fc);
        CHECK(fc.light_count == 5);
        for (auto i = 0; i < 4; ++i)
        {
            CHECK(fc.path_offset[i] == table.path_offset[i]);
            CHECK(fc.path_count[i] == table.path_count[i]);
        }
    }

    SECTION("no lights is an empty table, which the frame block reads as lit by the environment alone")
    {
        auto const empty = sv::pt_light_table::grouped({});
        auto fc = sv::pt_frame_constants_gpu{};
        empty.describe_in(fc);
        CHECK(empty.records.empty());
        CHECK(fc.light_count == 0);
    }
}

// The accumulation restarts when a hash of the uploaded bytes changes, and the lights are among them.
// So two lights built the same way must lay out to the same bytes, pads included — otherwise the image restarts every
// frame and never converges, which looks like noise rather than like a bug.
TEST("sv - equal lights lay out to identical bytes, and any change is seen in them")
{
    auto const make
        = [] { return sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 0.5f)).nits(12); };

    auto const a = sv::light_gpu::from(make());
    auto const b = sv::light_gpu::from(make());
    auto const bytes = [](sv::light_gpu const& g) { return cc::span<sv::light_gpu const>(&g, 1).as_bytes(); };

    CHECK(cc::make_hash_of_bytes(bytes(a)) == cc::make_hash_of_bytes(bytes(b)));

    auto dimmer = make();
    dimmer.nits(11);
    CHECK(cc::make_hash_of_bytes(bytes(sv::light_gpu::from(dimmer))) != cc::make_hash_of_bytes(bytes(a)));

    auto flipped = make();
    flipped.face(sv::light_face::back);
    CHECK(cc::make_hash_of_bytes(bytes(sv::light_gpu::from(flipped))) != cc::make_hash_of_bytes(bytes(a)));
}

TEST("sv - light_gpu::from converts every path to the quantity its estimator reads")
{
    using namespace tg::literals;
    auto const down = tg::vec3f(0, -1, 0);

    SECTION("a point is an intensity; a flux spreads over the whole sphere")
    {
        auto const candela = sv::light_gpu::from(sv::light::point(tg::pos3f(1, 2, 3)).candela(800));
        CHECK(candela.path == u32(sv::light_path::point));
        CHECK(near(candela.position, tg::vec3f(1, 2, 3)));
        CHECK(near(candela.emission, tg::vec3f(800, 800, 800)));

        // Unshaped: the falloff is a constant 1 whatever the direction.
        CHECK(candela.cone_scale == 0.0f);
        CHECK(candela.cone_offset == 1.0f);

        auto const lumen = sv::light_gpu::from(sv::light::point(tg::pos3f(0, 0, 0)).lumens(4.0f * tg::pi<f32> * 100));
        CHECK(near(lumen.emission, tg::vec3f(100, 100, 100)));
    }

    SECTION("a spot's cone is glTF's falloff, full inside the inner angle and zero past the outer")
    {
        auto const g = sv::light_gpu::from(sv::light::spot(tg::pos3f(0, 3, 0), down, 30_deg_f, 20_deg_f).candela(1));
        CHECK(near(g.normal, down));

        auto const falloff = [&](tg::angle_f a)
        {
            auto const x = cc::clamp(tg::cos(a) * g.cone_scale + g.cone_offset, 0.0f, 1.0f);
            return x * x;
        };
        CHECK(tg::abs(falloff(10_deg_f) - 1.0f) < 1e-5f);
        CHECK(tg::abs(falloff(20_deg_f) - 1.0f) < 1e-4f);
        CHECK(falloff(25_deg_f) > 0.0f);
        CHECK(falloff(25_deg_f) < 1.0f);
        CHECK(falloff(31_deg_f) == 0.0f);
    }

    SECTION("a directional light is an irradiance along the direction it travels")
    {
        auto const g = sv::light_gpu::from(sv::light::directional(down).lux(5));
        CHECK(g.path == u32(sv::light_path::distant_point));
        CHECK(near(g.normal, down));
        CHECK(near(g.emission, tg::vec3f(5, 5, 5)));
    }

    SECTION("a sun is a disc of radiance that delivers its illuminance to a surface facing it")
    {
        auto const g = sv::light_gpu::from(sv::light::sun(down, 2_deg_f).lux(10));
        CHECK(g.path == u32(sv::light_path::distant_disc));
        CHECK(tg::abs(g.one_minus_cos_angular_radius - 1.5230484e-4f) < 1e-9f); // 1 - cos(1 deg), taken in double

        // pi * sin(r)^2 * L back to the lux it was given.
        auto const sin_r = tg::sin(1_deg_f);
        CHECK(tg::abs(tg::pi<f32> * sin_r * sin_r * g.emission[0] - 10.0f) < 1e-3f);

        // The shader's projected solid angle, pi * (1 - cos) * (1 + cos), has to agree with the pi * sin^2 the lux was
        // divided by, even for a disc far smaller than the sun — where a stored cosine would round most of it away.
        for (auto const diameter : {0.53f, 0.1f, 0.02f})
        {
            auto const small = sv::light_gpu::from(sv::light::sun(down, tg::angle_f::make_from_degree(diameter)).lux(10));
            auto const omc = small.one_minus_cos_angular_radius;
            auto const delivered = tg::pi<f32> * omc * (2.0f - omc) * small.emission[0];
            CHECK(tg::abs(delivered - 10.0f) < 1e-3f * 10.0f);
        }
    }

    SECTION("a two-sided rect sets its flag, and its flux is shared between both faces")
    {
        auto const rect = [] { return sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 0.5f)); };
        auto one = rect();
        one.lumens(tg::pi<f32> * 2.0f * 12);
        auto both = rect();
        both.face(sv::light_face::both).lumens(tg::pi<f32> * 2.0f * 12);

        CHECK(sv::light_gpu::from(one).flags == 0u);
        CHECK((sv::light_gpu::from(both).flags & sv::light_gpu::flag_two_sided) != 0u);
        CHECK(near(sv::light_gpu::from(one).emission, tg::vec3f(12, 12, 12)));
        CHECK(near(sv::light_gpu::from(both).emission, tg::vec3f(6, 6, 6)));
    }

    SECTION("a spread is a hard-edged cone about the rect's face")
    {
        auto l = sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 1));
        l.spread(20_deg_f);
        auto const g = sv::light_gpu::from(l);
        auto const falloff = [&](tg::angle_f a)
        {
            auto const x = cc::clamp(tg::cos(a) * g.cone_scale + g.cone_offset, 0.0f, 1.0f);
            return x * x;
        };
        CHECK(falloff(10_deg_f) == 1.0f);
        CHECK(falloff(30_deg_f) == 0.0f);
    }

    // The width between the two angles is clamped only to keep a hard edge finite.
    // Clamped too wide, the ramp's slope is capped and a narrow cone never reaches full intensity on its own axis.
    SECTION("a narrow cone still reaches full intensity on its axis")
    {
        auto const on_axis = [](sv::light_gpu const& g)
        {
            auto const x = cc::clamp(g.cone_scale + g.cone_offset, 0.0f, 1.0f); // cos = 1
            return x * x;
        };

        auto tight = sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 1));
        tight.spread(1_deg_f);
        CHECK(on_axis(sv::light_gpu::from(tight)) == 1.0f);

        // Inner defaults to 0, so the ramp meets 1 exactly on the axis, up to rounding.
        CHECK(on_axis(sv::light_gpu::from(sv::light::spot(tg::pos3f(0, 3, 0), down, 2_deg_f).candela(1))) > 0.999f);
    }
}

TEST("sv - a layer nobody lit falls back to a sun, which it can turn off")
{
    auto const l = sv::layer{};
    REQUIRE(l.fallback_light.has_value());
    CHECK(l.fallback_light.value().path() == sv::light_path::distant_disc);
    CHECK(sv::light_problem(l.fallback_light.value()).empty());
}

// The daylight preset used to fold a soft sun lobe into its sky, and now hands a real sun back beside it.
// If the sky still carried its lobe, a scene set up with both would be lit by the sun twice.
TEST("sv - daylight's sky carries no sun, so the pair counts it once")
{
    auto const day = sv::daylight();

    // A gradient lives in bands 0 and 1 alone; any band-2 energy is a lobe.
    for (auto i = 4; i < sv::background::sh_coefficient_count; ++i)
        CHECK(day.sky.sh[i] == tg::vec3f(0, 0, 0));

    // The sun is a light, high in the sky and travelling down.
    CHECK(day.sun.path() == sv::light_path::distant_disc);
    CHECK(sv::light_problem(day.sun).empty());
    CHECK(sv::light_gpu::from(day.sun).normal[1] < 0.0f);
}

TEST("sv - a light's camera visibility and shadowing reach its GPU record, with 0 as the default")
{
    auto const rect = [] { return sv::light::rect(tg::pos3f(0, 3, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 1)); };

    // An ordinary light sets neither bit, so a zeroed record behaves as one.
    auto const plain = sv::light_gpu::from(rect());
    CHECK((plain.flags & sv::light_gpu::flag_visible_to_camera) == 0u);
    CHECK((plain.flags & sv::light_gpu::flag_casts_no_shadow) == 0u);
    CHECK(plain.link_mask == ~0u); // reserved, and "every instance" meanwhile

    auto seen = rect();
    seen.visible_to_camera().casts_shadows(false);
    auto const g = sv::light_gpu::from(seen);
    CHECK((g.flags & sv::light_gpu::flag_visible_to_camera) != 0u);
    CHECK((g.flags & sv::light_gpu::flag_casts_no_shadow) != 0u);

    // A sun has an extent and may be seen; a point or a parallel light has none, and asserts.
    auto sun = sv::light::sun(tg::vec3f(0, -1, 0));
    CHECK(sv::light_problem(sun.visible_to_camera()).empty());
    CHECK_ASSERTS(sv::light::point(tg::pos3f(0, 0, 0)).visible_to_camera());
    CHECK_ASSERTS(sv::light::directional(tg::vec3f(0, -1, 0)).visible_to_camera());

    // Any light may cast no shadow.
    CHECK(sv::light_problem(sv::light::point(tg::pos3f(0, 0, 0)).casts_shadows(false)).empty());
}
