#include "check-test-support.hh"

using namespace sgl_test;

// The binding model of libs/graphics/shaped-graphics-language/docs/spec/bindings.md, as far as the compiler carries it.
// Buffers, textures, images and samplers are built; bytes and separate constant buffers parse and are reported.

namespace
{
/// A binding and an entry point that lists it, since a binding nobody demands is never checked.
cc::string listing(cc::string_view members, cc::string_view body = "")
{
    return cc::format("binding work:\n"
                      "{}"
                      "\n"
                      "struct pixel_input:\n"
                      "    @position position: hpos4\n"
                      "\n"
                      "@pixel struct target:\n"
                      "    color: float4\n"
                      "\n"
                      "@pixel fun main_ps(p: pixel_input){{work}} -> target:\n"
                      "{}"
                      "    return {{color = float4(1.0, 1.0, 1.0, 1.0)}}\n",
                      members, body);
}
} // namespace

TEST("sgl check - a buffer member and its mut form are types")
{
    CHECK(reports_for(listing("    scale: float\n"
                              "    src: buffer[float]\n"
                              "    dst: mut buffer[float]\n"
                              "    pos: buffer[float3]\n"))
          == "");
}

TEST("sgl check - what a buffer's element may be")
{
    // A struct element needs a layout rule the four targets agree on, which the spec's bindings file leaves open.
    CHECK(reports_for(cc::string("struct particle:\n    mass: float\n\n") + listing("    items: buffer[particle]\n"))
              .contains("a buffer of anything but a scalar or a vector"));

    CHECK(reports_for(listing("    x: buffer[float, int]\n")).contains("a buffer takes one element type"));
    CHECK(reports_for(listing("    x: buffer[not_a_type]\n")).contains("unknown-name"));
}

TEST("sgl check - bytes and a separate constant buffer are not built yet")
{
    CHECK(reports_for(listing("    raw: bytes\n")).contains("unknown-name"));
    CHECK(reports_for(listing("    params: constants[dispatch_params]\n")).contains("type arguments"));

    // `mut` says a resource may be written, so it says nothing about a value.
    CHECK(reports_for(listing("    wrong: mut float\n")).contains("only a resource may be `mut`"));
}

TEST("sgl check - every texture, image and sampler form sg binds is a binding member")
{
    constexpr auto members = "    src: texture2d[float4]\n"
                             "    ids: texture2d_array[uint]\n"
                             "    sky: texture_cube[float3]\n"
                             "    shadow: texture2d_depth\n"
                             "    @unfilterable positions: texture2d[float4]\n"
                             "    ro: image2d[.rgba16_float]\n"
                             "    acc: mut image2d[.r32_float]\n"
                             "    dst: out image3d[.rgba8_unorm]\n"
                             "    smp: sampler\n"
                             "    cmp: comparison_sampler\n"
                             "    @non_filtering near: sampler\n"
                             "    sampler bilinear:\n"
                             "        filter = .linear\n"
                             "        address = .clamp_edge\n"
                             "        max_anisotropy = 8\n";
    CHECK(reports_for(listing(members)) == "");
}

TEST("sgl check - a resource that some backend lacks needs a feature, and a misplaced word is an error")
{
    // CHK-196: refused by the feature that would grant it, on every target alike.
    CHECK(reports_for(listing("    a: mut image2d[.rgba8_unorm]\n")).contains("needs-feature"));
    CHECK(reports_for(listing("    a: mut image2d[.rgba8_unorm]\n")).contains("readwrite_storage_formats"));
    CHECK(reports_for(listing("    a: out image2d[.r8_unorm]\n")).contains("extended_storage_formats"));
    CHECK(reports_for(listing("    a: texture2d_ms_array[float4]\n")).contains("multisampled arrays"));

    CHECK(reports_for(listing("    a: out texture2d[float4]\n")).contains("a texture is only ever read"));
    CHECK(reports_for(listing("    a: out buffer[float]\n")).contains("a buffer is never `out`"));
    CHECK(reports_for(listing("    a: image2d[float4]\n")).contains("one of sg's storage formats"));
    CHECK(reports_for(listing("    a: image2d[.rgba7_unorm]\n")).contains("one of sg's storage formats"));
    CHECK(reports_for(listing("    a: texture2d[vec3]\n")).contains("a float, an int or a uint"));
    CHECK(reports_for(listing("    a: texture2d\n")).contains("`texture2d[float4]`"));
    CHECK(reports_for(listing("    @unfilterable a: texture2d[uint]\n")).contains("only a texture of floats"));
    CHECK(reports_for(listing("    @non_filtering a: comparison_sampler\n")).contains("only a `sampler` member"));
    CHECK(reports_for(listing("    sampler s:\n        filter = .cubic\n"))
              .contains("filter takes one of .nearest, .linear"));
    CHECK(reports_for(listing("    sampler s:\n        border = .black\n")).contains("border is no sampler setting"));
    CHECK(reports_for("@inline binding c:\n    x: float\n    sampler s:\n        filter = .linear\n")
              .contains("an @inline binding holds constants only"));
}

TEST("sgl check - a sampler block's settings and attributes are judged")
{
    constexpr auto kind = "invalid-attribute-arguments";
    constexpr auto range = "max_anisotropy takes an int from 1 to 16";
    auto const anisotropy = [](cc::string_view value)
    {
        return reports_for(
            listing(cc::format("    sampler s:\n        filter = .linear\n        max_anisotropy = {}\n", value)));
    };

    CHECK(anisotropy("1") == "");
    CHECK(anisotropy("16") == "");
    for (auto const bad : {"0", "-2", "17", "2.5", "1e10", "99999999999", ".linear"})
    {
        CHECK(anisotropy(bad).contains(kind));
        CHECK(anisotropy(bad).contains(range));
    }

    // CHK-207: anisotropy needs every filter linear, since WebGPU refuses it otherwise.
    CHECK(reports_for(listing("    sampler s:\n        filter = .linear\n        mip_filter = .nearest\n"
                              "        max_anisotropy = 4\n"))
              .contains("max_anisotropy above 1 needs every filter .linear"));
    CHECK(reports_for(listing("    sampler s:\n        filter = .nearest\n        max_anisotropy = 1\n")) == "");

    // A sign on a number is part of the literal, so a negative bias is a number like any other.
    CHECK(reports_for(listing("    sampler s:\n        mip_lod_bias = -0.5\n")) == "");

    // An attribute on the block is judged as on any member, rather than dropped.
    CHECK(reports_for(listing("    @whatever sampler s:\n        filter = .linear\n"))
              .contains("the attribute @whatever on a binding member"));

    CHECK(reports_for(listing("    s: mut sampler\n")).contains("a sampler is never `mut`"));
}

TEST("sgl check - a repeated sampler setting overrides the one before it")
{
    auto const checked = check_sources(read_prelude(), listing("    sampler s:\n"
                                                               "        filter = .linear\n"
                                                               "        filter = .nearest\n"
                                                               "        address_u = .clamp_edge\n"
                                                               "        address = .mirror_repeat\n"));
    CHECK(reports_of(checked) == "");
    REQUIRE(checked.module.samplers.size() == 1);
    auto const& s = checked.module.samplers[0];
    CHECK(s.min_filter == 0);
    CHECK(s.mag_filter == 0);
    CHECK(s.mip_filter == 0);
    CHECK(s.address_u == 1);
    CHECK(s.address_w == 1);

    // Anisotropy is judged on the final filters, so the order decides.
    CHECK(reports_for(listing("    sampler s:\n        filter = .nearest\n        max_anisotropy = 4\n"
                              "        filter = .linear\n"))
          == "");
    CHECK(reports_for(listing("    sampler s:\n        filter = .linear\n        max_anisotropy = 4\n"
                              "        filter = .nearest\n"))
              .contains("max_anisotropy above 1 needs every filter .linear"));
}

TEST("sgl check - an @unfilterable texture is sampled only through a sampler that never filters")
{
    constexpr auto sample = "    let c = DEBUG_sample_level(work.t, float2(0.5, 0.5), 0.0, work.s)\n";
    auto const reports = [&](cc::string_view sampler)
    { return reports_for(listing(cc::format("    @unfilterable t: texture2d[float4]\n{}", sampler), sample)); };

    CHECK(reports("    @non_filtering s: sampler\n") == "");
    CHECK(reports("    sampler s:\n        filter = .nearest\n") == "");

    constexpr auto refused = "work.t is @unfilterable, and work.s filters";
    CHECK(reports("    s: sampler\n").contains("type-mismatch"));
    CHECK(reports("    s: sampler\n").contains(refused));
    CHECK(reports("    sampler s:\n        address = .repeat\n").contains(refused));
    CHECK(reports("    sampler s:\n        filter = .nearest\n        mag_filter = .linear\n").contains(refused));

    // A texture that may be filtered takes either kind.
    CHECK(reports_for(listing("    t: texture2d[float4]\n    s: sampler\n", sample)) == "");
}

TEST("sgl check - a texture, an image or a sampler is handed to a builtin and is no value otherwise")
{
    constexpr auto members = "    src: texture2d[float4]\n"
                             "    ro: image2d[.rgba8_unorm]\n"
                             "    dst: out image2d[.rgba8_unorm]\n"
                             "    smp: sampler\n";
    CHECK(reports_for(listing(members, "    let c = DEBUG_sample_level(work.src, float2(0.5, 0.5), 0.0, work.smp)\n"
                                       "    DEBUG_store(work.dst, int2(0, 0), c + DEBUG_load(work.ro, int2(0, 0)))\n"))
          == "");

    CHECK(reports_for(listing(members, "    let t = work.src\n")).contains("texture2d[float4] as a value"));
    // CHK-202: a read-only image cannot be stored to, and a write-only one cannot be loaded.
    CHECK(reports_for(listing(members, "    DEBUG_store(work.ro, int2(0, 0), float4(1.0, 1.0, 1.0, 1.0))\n"))
              .contains("no-matching-overload"));
    CHECK(reports_for(listing(members, "    let v = DEBUG_load(work.dst, int2(0, 0))\n")).contains("no-matching-overload"));
    // A texel of four floats is what an rgba8 image holds, and nothing narrower.
    CHECK(reports_for(listing(members, "    DEBUG_store(work.dst, int2(0, 0), 1.0)\n")).contains("no-matching-overload"));
}

TEST("sgl check - a buffer element is read by subscript and written where the buffer is mut")
{
    constexpr auto members = "    src: buffer[float]\n    dst: mut buffer[float]\n";

    CHECK(reports_for(listing(members, "    let v = work.src[0]\n    work.dst[1] = v * 2.0\n")) == "");

    // A read-only buffer is the one `not-assignable` the binding model adds.
    CHECK(reports_for(listing(members, "    work.src[0] = 1.0\n")).contains("this buffer is read-only"));
    CHECK(reports_for(listing(members, "    let v = work.src[1.5]\n")).contains("a buffer is indexed by an int"));

    // Everything else a subscript could mean is still unbuilt, and says so rather than guessing.
    CHECK(reports_for(listing(members, "    let v = p.position[0]\n")).contains("a subscript on anything but a buffer"));
}

TEST("sgl check - a buffer is a resource and never a value")
{
    constexpr auto members = "    src: buffer[float]\n    dst: mut buffer[float]\n";
    constexpr auto value = "a buffer as a value";

    // Read through a subscript and nowhere else: not bound to a local, not passed, not returned.
    CHECK(reports_for(listing(members, "    let b = work.src\n")).contains(value));
    CHECK(reports_for(cc::string("fun first(b: buffer[float]) -> float => b[0]\n\n") + listing(members)).contains(value));
    CHECK(reports_for(cc::string("fun pass(x: float) -> buffer[float] => x\n\n") + listing(members)).contains(value));
    CHECK(reports_for(listing(members, "    let b : buffer[float] = work.src\n")).contains(value));

    // Nor a field of a struct, which no target can hold; relaxing that needs a rule for hoisting it (the spec's bindings file).
    CHECK(reports_for(cc::string("struct view:\n    items: buffer[float]\n\n") + listing(members)).contains(value));
}

TEST("sgl check - a compute entry point takes the thread id and returns nothing")
{
    constexpr auto work = "binding work:\n    values: mut buffer[float]\n\n";

    CHECK(reports_for(cc::string(work)
                      + "@compute(64) fun go(@thread_id id: int3){work}:\n"
                        "    work.values[id.x] = 1.0\n")
          == "");

    // The struct spelling checks too; only the emitter has not caught up.
    CHECK(reports_for(cc::string(work)
                      + "struct dispatch:\n    @thread_id id: int3\n\n"
                        "@compute(64) fun go(d: dispatch){work}:\n"
                        "    work.values[d.id.x] = 1.0\n")
          == "");

    CHECK(reports_for(cc::string(work)
                      + "@compute(64) fun go(@thread_id id: float3){work}:\n"
                        "    work.values[0] = 1.0\n")
              .contains("a @thread_id parameter is an int3"));

    CHECK(reports_for(cc::string(work)
                      + "@compute(64) fun go(@thread_id id: int3){work} -> int:\n"
                        "    return 1\n")
              .contains("a @compute fun returns nothing"));

    CHECK(reports_for(cc::string(work)
                      + "@compute fun go(@thread_id id: int3){work}:\n"
                        "    work.values[0] = 1.0\n")
              .contains("@compute takes one to three workgroup sizes"));

    CHECK(reports_for(cc::string(work)
                      + "@compute(0) fun go(@thread_id id: int3){work}:\n"
                        "    work.values[0] = 1.0\n")
              .contains("a workgroup size is a positive int literal"));
}

TEST("sgl check - a buffer's host name is its path, so no two buffers of a module share one (CHK-171)")
{
    // `a_b.c` and `a.b_c` would share an identifier in any target, and the host never sees one.
    auto const two_groups = cc::string("binding work_a:\n    b: buffer[float]\n\n") + listing("    a_b: buffer[float]\n");
    CHECK(reports_for(two_groups) == "");
    // Nor does a module-level declaration's name matter to a buffer.
    CHECK(reports_for(cc::string("fun work_src() -> float => 1.0\n\n") + listing("    src: buffer[float]\n")) == "");
}
