#include "viewer_test_env.hh"

#include <clean-core/common/macros.hh>  // CC_ARCH_ARM64
#include <clean-core/common/utility.hh> // cc::clamp, cc::max
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/resources/quadric_data.hh>
#include <shaped-viewer/scene/quadric.hh>
#include <sv_test_shaders.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::normalize
#include <typed-geometry/scalar/scalar.hh>  // tg::abs

using namespace cc::primitive_defines;

// The material and quadric runtimes, held to CPU references.
//
// `tests/shaders/runtime_probe.sgl` calls the real `quadric.intersect` and `quadric.roots_of` of module `quadric`, and the
// real attribute loads of module `material`.
// The quadric root solve is why this is a probe rather than a compile check: it is the numerically delicate half, and an
// edit that formed one product in another order would move a grazing hit or lose a small primitive seen from afar.
// So the cases lean on exactly those: grazing rays, a sphere a hundredth of a unit across seen from a hundred units, a
// ray down a cylinder's axis where the quadratic term vanishes, and a plane whose equation is linear everywhere.

namespace
{
/// Mirrors `runtime_probe_case` in tests/shaders/runtime_probe.sgl.
struct runtime_probe_case
{
    tg::vec3f origin;
    u32 quadric = 0;
    tg::vec3f dir;
    float t_min = 0.0f;
    float t_max = 1e30f;
    u32 primitive = 0;
    u32 is_indexed = 0;
    u32 desc_offset = 0;
    tg::vec2f bary;
    u32 pad[2] = {};
};

static_assert(sizeof(sv_test::shaders::runtime_probe_case) == sizeof(runtime_probe_case),
              "runtime_probe_case must match runtime_probe_case in tests/shaders/runtime_probe.sgl");

/// How many float4s one case writes, in the order the probe writes them.
constexpr isize results_per_case = 8;

/// The memory the probe reads: quadric records, attribute descriptors with their elements, and triangle indices.
struct runtime_probe_memory
{
    cc::vector<sv::quadric_gpu> quadrics;
    cc::vector<u32> attributes;
    cc::vector<u32> indices;
};

/// One descriptor, as `load_attribute_desc` reads it: buffer, offset, stride.
void push_desc(cc::vector<u32>& words, u32 offset, u32 stride)
{
    words.push_back(0);
    words.push_back(offset);
    words.push_back(stride);
}

/// The attribute elements, twelve of each, so that four triangles read per vertex or per corner stay in range.
/// Two descriptor blocks: at byte 0 the rotations are real, at byte 48 they are all zero, which is the no-rotation fallback.
cc::vector<u32> make_attributes()
{
    auto constexpr element_count = 12;
    auto constexpr descs_bytes = 96;
    auto constexpr f1_offset = descs_bytes;
    auto constexpr f2_offset = f1_offset + element_count * 4;
    auto constexpr f3_offset = f2_offset + element_count * 8;
    auto constexpr f4_offset = f3_offset + element_count * 16;
    auto constexpr zero_offset = f4_offset + element_count * 16;

    auto words = cc::vector<u32>();
    push_desc(words, f1_offset, 4);
    push_desc(words, f2_offset, 8);
    push_desc(words, f3_offset, 16); // a float3 padded to 16, as a stride need not equal the element's size
    push_desc(words, f4_offset, 16);

    push_desc(words, f1_offset, 4);
    push_desc(words, f2_offset, 8);
    push_desc(words, f3_offset, 16);
    push_desc(words, zero_offset, 16);

    auto const push_float = [&](float f) { words.push_back(cc::bit_cast<u32>(f)); };

    for (auto i = 0; i < element_count; ++i)
        push_float(0.5f + 1.25f * float(i));
    for (auto i = 0; i < element_count; ++i)
    {
        push_float(float(i) * 0.1f);
        push_float(1.0f - float(i) * 0.07f);
    }
    for (auto i = 0; i < element_count; ++i)
    {
        push_float(float(i));
        push_float(-0.3f * float(i));
        push_float(2.0f + 0.01f * float(i));
        push_float(0.0f);
    }
    // Unit quaternions near one another, every third one negated: the same rotation, antipodal in 4D.
    for (auto i = 0; i < element_count; ++i)
    {
        auto const sign = i % 3 == 1 ? -1.0f : 1.0f;
        auto const q = tg::normalize(tg::vec4f(0.02f * float(i), 0.1f, -0.03f * float(i), 1.0f)) * sign;
        for (auto lane = 0; lane < 4; ++lane)
            push_float(q[lane]);
    }
    for (auto i = 0; i < element_count * 4; ++i)
        push_float(0.0f);

    CC_ASSERT(isize(words.size()) * 4 == zero_offset + element_count * 16, "the attribute layout above is miscounted");
    return words;
}

/// The primitives every ray below is aimed at, by index.
enum : u32
{
    q_sphere = 0,
    q_tiny_far_sphere,
    q_capped_cylinder,
    q_open_cylinder,
    q_cone,
    q_plane_disc,
    q_count,
};

cc::vector<sv::quadric_primitive> make_primitives()
{
    auto out = cc::vector<sv::quadric_primitive>();
    out.push_back(sv::quadric_primitive::create_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f)));
    out.push_back(sv::quadric_primitive::create_sphere(tg::sphere3f(tg::pos3f(100, 50, 0), 0.01f)));
    out.push_back(
        sv::quadric_primitive::create_cylinder(tg::segment3f(tg::pos3f(0, 0, -1), tg::pos3f(0, 0, 1)), 0.5f, true));
    out.push_back(
        sv::quadric_primitive::create_cylinder(tg::segment3f(tg::pos3f(0, 0, -1), tg::pos3f(0, 0, 1)), 0.5f, false));
    out.push_back(sv::quadric_primitive::create_cone(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 2)), 1.0f, true));

    // The plane z = 0, whose quadratic term is zero for every ray: the linear root and nothing else, clipped to a disc.
    out.push_back({.origin = tg::pos3f(0, 0, 0),
                   .surface = {.linear = tg::vec3f(0, 0, 0.5f)},
                   .clip = sv::quadric3::sphere_about_origin(3.0f)});
    CC_ASSERT(out.size() == q_count, "make_primitives and the q_ enum disagree");
    return out;
}

/// The rays, each a (quadric, origin, direction, t_min, t_max) the root solve is delicate on.
cc::vector<runtime_probe_case> make_rays()
{
    auto const ray = [](u32 q, tg::vec3f o, tg::vec3f d, float t_min = 0.0f, float t_max = 1e30f)
    { return runtime_probe_case{.origin = o, .quadric = q, .dir = tg::normalize(d), .t_min = t_min, .t_max = t_max}; };

    auto rays = cc::vector<runtime_probe_case>();

    // Through the center, grazing, just missing, from inside, and with each root cut away by the interval.
    rays.push_back(ray(q_sphere, tg::vec3f(-5, 0, 0), tg::vec3f(1, 0, 0)));
    rays.push_back(ray(q_sphere, tg::vec3f(-5, 0.999f, 0), tg::vec3f(1, 0, 0)));
    rays.push_back(ray(q_sphere, tg::vec3f(-5, 0.99999f, 0), tg::vec3f(1, 0, 0)));
    rays.push_back(ray(q_sphere, tg::vec3f(-5, 1.001f, 0), tg::vec3f(1, 0, 0)));
    rays.push_back(ray(q_sphere, tg::vec3f(0, 0, 0), tg::vec3f(0.3f, 1, -0.2f)));
    rays.push_back(ray(q_sphere, tg::vec3f(-5, 0.2f, 0.1f), tg::vec3f(1, 0, 0), 5.0f));
    rays.push_back(ray(q_sphere, tg::vec3f(-5, 0.2f, 0.1f), tg::vec3f(1, 0, 0), 0.0f, 3.0f));
    rays.push_back(ray(q_sphere, tg::vec3f(3, 4, 5), tg::vec3f(-3, -4, -5.1f)));

    // A hundredth of a unit across, from the world origin a hundred and some units away: where the shift to the closest
    // approach is a precision requirement rather than a simplification.
    auto const far_center = tg::vec3f(100, 50, 0);
    rays.push_back(ray(q_tiny_far_sphere, tg::vec3f(0, 0, 0), far_center));
    rays.push_back(ray(q_tiny_far_sphere, tg::vec3f(0, 0, 0), far_center + tg::vec3f(0, 0, 0.0099f)));
    rays.push_back(ray(q_tiny_far_sphere, tg::vec3f(0, 0, 0), far_center + tg::vec3f(0, 0, 0.0101f)));
    rays.push_back(ray(q_tiny_far_sphere, tg::vec3f(0, 0.004f, 0), far_center + tg::vec3f(0.003f, 0, 0.007f)));

    // Down the axis, where the side's quadratic term vanishes and only the caps can answer; and across, and slanted.
    for (auto const q : {q_capped_cylinder, q_open_cylinder})
    {
        rays.push_back(ray(q, tg::vec3f(0, 0, -5), tg::vec3f(0, 0, 1)));
        rays.push_back(ray(q, tg::vec3f(0.2f, 0.1f, -5), tg::vec3f(0, 0, 1)));
        rays.push_back(ray(q, tg::vec3f(-5, 0, 0.3f), tg::vec3f(1, 0, 0)));
        rays.push_back(ray(q, tg::vec3f(-5, 0.499f, 0), tg::vec3f(1, 0, 0)));
        rays.push_back(ray(q, tg::vec3f(-3, 0.1f, -3), tg::vec3f(1, 0, 1)));
        rays.push_back(ray(q, tg::vec3f(0, 0, 0), tg::vec3f(0.1f, 0.2f, 1)));
    }

    // A cone: through its side, through its base, along a generator, and from its apex.
    rays.push_back(ray(q_cone, tg::vec3f(-5, 0, 0.5f), tg::vec3f(1, 0, 0)));
    rays.push_back(ray(q_cone, tg::vec3f(0.1f, 0.2f, -5), tg::vec3f(0, 0, 1)));
    rays.push_back(ray(q_cone, tg::vec3f(-1, 0, -0.5f), tg::vec3f(0.5f, 0, 1)));
    rays.push_back(ray(q_cone, tg::vec3f(0, 0, 4), tg::vec3f(0.2f, 0.1f, -1)));

    // The plane: the single linear root, inside the disc and outside it, and a ray in the plane with no root at all.
    rays.push_back(ray(q_plane_disc, tg::vec3f(0, 0, 5), tg::vec3f(0, 0, -1)));
    rays.push_back(ray(q_plane_disc, tg::vec3f(1, 1, 5), tg::vec3f(0.1f, -0.2f, -1)));
    rays.push_back(ray(q_plane_disc, tg::vec3f(5, 0, 5), tg::vec3f(0, 0, -1)));
    rays.push_back(ray(q_plane_disc, tg::vec3f(-5, 0, 0), tg::vec3f(1, 0, 0)));
    return rays;
}

/// Every ray, each paired with a triangle: indexed or not, its barycentrics, and one of the two descriptor blocks.
cc::vector<runtime_probe_case> make_cases()
{
    auto cases = make_rays();
    for (auto i = isize(0); i < cases.size(); ++i)
    {
        auto& c = cases[i];
        auto const fi = float(i);
        c.primitive = u32(i % 4);
        c.is_indexed = u32(i % 2);
        c.desc_offset = i % 5 == 0 ? 48u : 0u;
        c.bary = tg::vec2f(0.05f + 0.03f * float(i % 7), 0.6f - 0.04f * float(i % 11)) * (0.9f + 0.001f * fi);
    }
    return cases;
}

runtime_probe_memory make_memory()
{
    auto memory = runtime_probe_memory();
    for (auto const& p : make_primitives())
        memory.quadrics.push_back(sv::quadric_gpu::of(p));
    memory.attributes = make_attributes();
    // Four triangles over twelve vertices, in an order no non-indexed reading would produce.
    memory.indices = cc::vector<u32>{11, 3, 7, 0, 10, 5, 9, 2, 4, 8, 1, 6};
    return memory;
}

/// Dispatches `cases` through the probe and reads back `results_per_case` float4s per case.
cc::shared_async<cc::vector<tg::vec4f>> run_probe(sg::context& ctx,
                                                  runtime_probe_memory const& memory,
                                                  cc::span<runtime_probe_case const> cases)
{
    auto const& entry = sv_test::shaders::runtime_probe.runtime_measure;
    auto const compiled_shader = entry->acquire(ctx);
    co_await cc::async_settled(compiled_shader);
    if (compiled_shader->has_error())
        FAIL(cc::format("the runtime probe shader did not compile:\n{}",
                        compiled_shader->try_error()->underlying().to_string()));

    auto const* const compiled = compiled_shader->try_value();
    REQUIRE(compiled != nullptr); // without it every check below is vacuous

    auto const group_layout = ctx.cached.acquire_binding_group_layout<sv_test::shaders::runtime_probe_io>();
    auto pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = entry.acquire_layout(ctx)});
    auto const built = co_await pipeline;
    REQUIRE(built != nullptr);

    auto cmd = ctx.create_command_list();

    auto const upload = [&]<class T>(cc::span<T const> data)
    {
        auto const buffer = ctx.transient.create_buffer<T>(
            data.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        cmd->upload.data_to_buffer(buffer, data);
        return buffer;
    };

    auto const case_buffer = upload(cases);
    auto const quadric_buffer = upload(cc::span<sv::quadric_gpu const>(memory.quadrics)).reinterpret_as<byte>();
    auto const attribute_buffer = upload(cc::span<u32 const>(memory.attributes)).reinterpret_as<byte>();
    auto const index_buffer = upload(cc::span<u32 const>(memory.indices)).reinterpret_as<byte>();

    auto const result_count = cases.size() * results_per_case;
    auto const result_buffer = ctx.transient.create_buffer<tg::vec4f>(
        result_count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    cmd->compute.bind_pipeline(*built);
    // SGL reads no buffer's length, so the probe is told the count.
    auto const group = ctx.transient.create_binding_group(
        *cmd, group_layout,
        sv_test::shaders::runtime_probe_io{
            .cases = case_buffer.reinterpret_as<sv_test::shaders::runtime_probe_case>().as_readonly_buffer(),
            .quadrics = quadric_buffer.as_readonly_buffer(),
            .attributes = attribute_buffer.as_readonly_buffer(),
            .indices = index_buffer.as_readonly_buffer(),
            .results = result_buffer.as_readwrite_buffer(),
            .count = u32(cases.size())});
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(cases.size());

    auto readback = cmd->download.data_from_buffer(result_buffer);

    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const items = co_await readback.data();
    REQUIRE(items.size() == result_count);

    auto out = cc::vector<tg::vec4f>();
    out.reserve(result_count);
    for (auto const& i : items)
        out.push_back(i);
    co_return out;
}
} // namespace

namespace
{
/// Element `i` of the attribute streams `make_attributes` writes, as the probe reads them.
[[nodiscard]] f32 f1_element(u32 i)
{
    return 0.5f + 1.25f * float(i);
}

[[nodiscard]] tg::vec3f f3_element(u32 i)
{
    return tg::vec3f(float(i), -0.3f * float(i), 2.0f + 0.01f * float(i));
}

[[nodiscard]] bool near(f32 a, f32 b, f32 eps = 1e-5f)
{
    return tg::abs(a - b) <= eps * cc::max(1.0f, tg::abs(b));
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - the quadric and material runtimes compute what their CPU references do",
                     (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    if (!sv_test::shared_env().has_compiler)
        SKIP("no DXC compiler to build the probe shader");

    auto const memory = make_memory();
    auto const cases = make_cases();
    auto const primitives = make_primitives();

    auto const results = co_await run_probe(ctx, memory, cases);
    REQUIRE(results.size() == cases.size() * results_per_case);

    // The root solve against the CPU reference it mirrors, and a share of hits so the probe is measuring something.
    auto hits = 0;
    for (auto i = isize(0); i < cases.size(); ++i)
    {
        auto const& c = cases[i];
        auto const reference = sv::intersect(
            primitives[c.quadric], tg::ray3f(tg::pos3f(c.origin[0], c.origin[1], c.origin[2]), c.dir), c.t_min, c.t_max);
        auto const gpu_valid = results[i * results_per_case + 1][0] != 0.0f;
        auto const gpu_t = results[i * results_per_case][0];
        CHECK(gpu_valid == reference.has_value())
            .context(cc::format("case {}: the GPU and the CPU reference disagree on a hit", i));
        if (gpu_valid && reference.has_value())
        {
            ++hits;
            CHECK(tg::abs(gpu_t - reference.value().t) <= 1e-4f * cc::max(1.0f, reference.value().t))
                .context(cc::format("case {}: t is {} on the GPU and {} on the CPU", i, gpu_t, reference.value().t));
        }
    }
    CHECK(hits >= isize(cases.size()) / 3).context(cc::format("only {} of {} rays hit", hits, cases.size()));

    // The attribute loads: the corners a triangle reads, and the per-vertex streams blended across them.
    for (auto i = isize(0); i < cases.size(); ++i)
    {
        auto const& c = cases[i];
        auto const* const r = &results[i * results_per_case];

        // An indexed triangle reads its corners through the index buffer, a plain list numbers them three per primitive.
        u32 corner[3] = {};
        for (auto k = 0; k < 3; ++k)
            corner[k] = c.is_indexed != 0 ? memory.indices[c.primitive * 3 + u32(k)] : c.primitive * 3 + u32(k);
        for (auto k = 0; k < 3; ++k)
            CHECK(r[6][k] == float(corner[k])).context(cc::format("case {}: corner {} is {}", i, k, r[6][k]));

        // The weights the hit hands the loads, whichever convention orders them, sum to one.
        auto const w = tg::vec3f(r[6][3], 1.0f - r[6][3] - r[7][2], r[7][2]);
        CHECK(near(w[0] + w[1] + w[2], 1.0f)).context(cc::format("case {}: the barycentrics do not sum to one", i));

        auto f1 = 0.0f;
        auto f3 = tg::vec3f(0, 0, 0);
        for (auto k = 0; k < 3; ++k)
        {
            f1 += w[k] * f1_element(corner[k]);
            f3 = f3 + f3_element(corner[k]) * w[k];
        }
        CHECK(near(r[3][3], f1, 1e-4f)).context(cc::format("case {}: f1 is {}, the CPU blend {}", i, r[3][3], f1));
        for (auto lane = 0; lane < 3; ++lane)
            CHECK(near(r[3][lane], f3[lane], 1e-4f))
                .context(cc::format("case {}: f3[{}] is {}, the CPU blend {}", i, lane, r[3][lane], f3[lane]));

        // The rotation blend aligns the corners into one hemisphere before it sums, so a blend of unit quaternions stays a
        // rotation even where every third one is stored negated.
        if (c.desc_offset == 0)
        {
            auto const q = tg::vec4f(r[5][0], r[5][1], r[5][2], r[5][3]);
            auto const length = tg::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            CHECK(near(length, 1.0f, 1e-4f)).context(cc::format("case {}: the blended rotation has length {}", i, length));
        }
    }
}
