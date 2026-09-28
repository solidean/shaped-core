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
    // A struct element is placed by the storage rule, which the emitter judges; the check pass takes any type.
    CHECK(reports_for(cc::string("struct particle:\n    mass: float\n\n") + listing("    items: buffer[particle]\n"))
          == "");

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
    constexpr auto members = "    src: texture_2d[float4]\n"
                             "    ids: texture_2d_array[uint]\n"
                             "    sky: texture_cube[float3]\n"
                             "    shadow: texture_2d_depth\n"
                             "    @unfilterable positions: texture_2d[float4]\n"
                             "    ro: image_2d[.rgba16_float]\n"
                             "    acc: mut image_2d[.r32_float]\n"
                             "    dst: out image_3d[.rgba8_unorm]\n"
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
    // CHK-201: refused by the feature that would grant it, on every target alike; feature-check-test has the grants.
    CHECK(reports_for(listing("    a: mut image_2d[.rgba8_unorm]\n")).contains("needs-feature"));
    CHECK(reports_for(listing("    a: mut image_2d[.rgba8_unorm]\n")).contains("readwrite_image_formats"));
    CHECK(reports_for(listing("    a: out image_2d[.r8_unorm]\n")).contains("extended_image_formats"));
    CHECK(reports_for(listing("    a: texture_2d_ms_array[float4]\n")).contains("multisampled_array_textures"));

    CHECK(reports_for(listing("    a: out texture_2d[float4]\n")).contains("a texture is only ever read"));
    CHECK(reports_for(listing("    a: out buffer[float]\n")).contains("a buffer is never `out`"));
    CHECK(reports_for(listing("    a: image_2d[float4]\n")).contains("one of sg's image formats"));
    CHECK(reports_for(listing("    a: image_2d[.rgba7_unorm]\n")).contains("one of sg's image formats"));
    CHECK(reports_for(listing("    a: texture_2d[vec3]\n")).contains("a float, an int or a uint"));
    CHECK(reports_for(listing("    a: texture_2d\n")).contains("`texture_2d[float4]`"));
    CHECK(reports_for(listing("    @unfilterable a: texture_2d[uint]\n")).contains("only a texture of floats"));
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

    // CHK-212: anisotropy needs every filter linear, since WebGPU refuses it otherwise.
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
    constexpr auto sample = "    let c = work.t.sample(float2(0.5, 0.5), work.s, level = 0.0)\n";
    auto const reports = [&](cc::string_view sampler)
    { return reports_for(listing(cc::format("    @unfilterable t: texture_2d[float4]\n{}", sampler), sample)); };

    CHECK(reports("    @non_filtering s: sampler\n") == "");
    CHECK(reports("    sampler s:\n        filter = .nearest\n") == "");

    constexpr auto refused = "work.t is @unfilterable, and work.s filters";
    CHECK(reports("    s: sampler\n").contains("type-mismatch"));
    CHECK(reports("    s: sampler\n").contains(refused));
    CHECK(reports("    sampler s:\n        address = .repeat\n").contains(refused));
    CHECK(reports("    sampler s:\n        filter = .nearest\n        mag_filter = .linear\n").contains(refused));

    // A texture that may be filtered takes either kind.
    CHECK(reports_for(listing("    t: texture_2d[float4]\n    s: sampler\n", sample)) == "");
}

TEST("sgl check - a texture, an image or a sampler is handed to a builtin and is no value otherwise")
{
    constexpr auto members = "    src: texture_2d[float4]\n"
                             "    ro: image_2d[.rgba8_unorm]\n"
                             "    dst: out image_2d[.rgba8_unorm]\n"
                             "    smp: sampler\n";
    CHECK(reports_for(listing(members, "    let c = work.src.sample(float2(0.5, 0.5), work.smp, level = 0.0)\n"
                                       "    work.dst.store(int2(0, 0), c + work.ro.load(int2(0, 0)))\n"))
          == "");

    CHECK(reports_for(listing(members, "    let t = work.src\n")).contains("texture_2d[float4] as a value"));
    // CHK-207: a read-only image cannot be stored to, and a write-only one cannot be loaded.
    CHECK(reports_for(listing(members, "    work.ro.store(int2(0, 0), float4(1.0, 1.0, 1.0, 1.0))\n"))
              .contains("no-matching-overload"));
    CHECK(reports_for(listing(members, "    let v = work.dst.load(int2(0, 0))\n")).contains("no-matching-overload"));
    // A texel of four floats is what an rgba8 image holds, and nothing narrower.
    CHECK(reports_for(listing(members, "    work.dst.store(int2(0, 0), 1.0)\n")).contains("no-matching-overload"));
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

    // CHK-271: a stage input is a parameter, never a struct field; the struct spelling was removed
    CHECK(reports_for(cc::string(work)
                      + "struct dispatch:\n    @thread_id id: int3\n\n"
                        "@compute(64) fun go(d: dispatch){work}:\n"
                        "    work.values[d.id.x] = 1.0\n")
              .contains("a @compute fun takes stage inputs alone"));
    // every id a dispatch hands over, in any order after one another
    CHECK(reports_for(cc::string(work)
                      + "@compute(64) fun go(@workgroup_id g: int3, @local_thread_index li: int, @thread_id id: int3, "
                        "@local_thread_id l: int3){work}:\n"
                        "    work.values[id.x] = (li + g.x + l.y) as float\n")
          == "");
    CHECK(reports_for(cc::string(work)
                      + "@compute(64) fun go(@thread_id a: int3, @thread_id b: int3){work}:\n"
                        "    work.values[a.x] = 1.0\n")
              .contains("@thread_id is taken twice"));
    CHECK(reports_for(cc::string(work)
                      + "@compute(64) fun go(@vertex_index v: int){work}:\n"
                        "    work.values[v] = 1.0\n")
              .contains("@vertex_index is an input of the vertex stage"));

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

TEST("sgl check - a @workgroup binding holds values a compute stage shares, within the portable budget")
{
    constexpr auto compute = "@compute(64) fun cs(@local_thread_index li: int){tile}:\n"
                             "    tile.values[li] = 1.0\n";
    CHECK(reports_for(cc::format("@workgroup binding tile:\n    values: float[64]\n    count: int\n\n{}", compute)) == "");

    CHECK(reports_for("@workgroup binding tile:\n    t: texture_2d[float4]\n")
              .contains("a @workgroup binding holds values the workgroup shares, and texture_2d[float4] is a "
                        "resource"));
    CHECK(reports_for("@workgroup binding tile:\n    sampler s:\n        filter = .linear\n")
              .contains("a @workgroup binding holds values the workgroup shares, and a sampler is none"));
    CHECK(reports_for("@inline @workgroup binding tile:\n    x: float\n")
              .contains("a binding is @inline constants or @workgroup memory, never both"));
    auto const padded = reports_for("@no_padding @workgroup binding tile:\n    x: float\n");
    CHECK(padded.contains("invalid-attribute-arguments"));
    CHECK(padded.contains("@no_padding guards a constant block's layout, and @workgroup memory has none a host sees"));

    // CHK-293: 16 KiB is what WebGPU gives by default and vulkan at least
    CHECK(reports_for("@workgroup binding tile:\n    values: float[4096]\n") == "");
    CHECK(reports_for("@workgroup binding tile:\n    values: float[4097]\n")
              .contains("tile holds 16388 bytes, and a workgroup has 16384 on every target"));
    // an atomic takes the four bytes of the int it holds
    CHECK(reports_for("@workgroup binding tile:\n    hits: atomic[uint][4096]\n") == "");
    CHECK(reports_for("@workgroup binding tile:\n    hits: atomic[uint][8192]\n")
              .contains("tile holds 32768 bytes, and a workgroup has 16384 on every target"));

    // CHK-294: only a compute stage has a workgroup
    auto const raster = reports_for("@workgroup binding tile:\n    values: float[4]\n\n"
                                    "struct pixel_input:\n    @position position: hpos4\n\n"
                                    "@pixel struct target:\n    color: float4\n\n"
                                    "@pixel fun ps(p: pixel_input){tile} -> target:\n"
                                    "    return {color = float4(1.0, 1.0, 1.0, 1.0)}\n");
    CHECK(raster.contains("tile is @workgroup memory, which only a compute stage has"));
}

TEST("sgl check - an atomic is memory a builtin updates, in a mut buffer or in workgroup memory, and never a value")
{
    CHECK(reports_for("binding stats:\n    hits: mut buffer[atomic[uint]]\n\n"
                      "@compute(64) fun cs(@local_thread_index li: int){stats}:\n"
                      "    stats.hits[0].add(1)\n")
          == "");

    constexpr auto value
        = "an atomic is read by `.load()` and written by `.store(v)` or one of its updates, and is never a value";
    auto const use = [](cc::string_view body)
    {
        return reports_for(cc::format("binding stats:\n    hits: mut buffer[atomic[uint]]\n\n"
                                      "@compute(64) fun cs(@local_thread_index li: int){{stats}}:\n{}",
                                      body));
    };
    CHECK(use("    let n = stats.hits[0]\n").contains(value));
    // an argument of an operator is handed to it, so what refuses it is the operator
    CHECK(use("    let n = stats.hits[0] + 1\n").contains("operator +(atomic[uint], 1)"));
    CHECK(use("    stats.hits[0] = 1\n").contains(value));

    CHECK(reports_for("binding stats:\n    hits: buffer[atomic[uint]]\n")
              .contains("a buffer of atomics is written by every update, so it is a `mut buffer`"));
    CHECK(reports_for("binding stats:\n    hits: atomic[uint]\n").contains("a constant block holds none"));
    CHECK(reports_for("binding stats:\n    hits: mut buffer[atomic[float]]\n")
              .contains("an atomic holds a `uint` or an `int`: `atomic[uint]`"));
    CHECK(reports_for("fun f() -> int:\n    let a: atomic[int] = 0\n    return 0\n")
              .contains("an atomic is memory in a `mut buffer` or a @workgroup binding, and never a value"));
}

TEST("sgl check - a binding array is one dimension of resources, bounded, granted, and read by element")
{
    constexpr auto granted = "require binding_arrays\n\n";
    CHECK(reports_for(cc::format("{}binding b:\n    t: texture_2d[float4][8]\n    i: out image_2d[.rgba8_unorm][2]\n"
                                 "    s: buffer[float][4]\n",
                                 granted))
          == "");
    CHECK(reports_for("binding b:\n    t: texture_2d[float4][8]\n").contains("needs binding_arrays"));
    CHECK(reports_for(cc::format("{}binding b:\n    t: texture_2d[float4][]\n", granted))
              .contains("an unbounded binding array, which sg binds none of yet"));
    CHECK(
        reports_for(cc::format("{}binding b:\n    s: comparison_sampler[4]\n", granted)).contains("an array of samplers"));
    // `sampler` is a keyword, and subscripted in a type position it reads as the name does (AST-135)
    CHECK(reports_for(cc::format("{}binding b:\n    s: sampler[2]\n", granted))
              .contains("unsupported-yet user:[sampler[2]] an array of samplers"));
    CHECK(reports_for(cc::format("{}binding b:\n    t: texture_2d[float4][2, 2]\n", granted))
              .contains("a binding array of more than one dimension"));

    // one resource is a plain member, which needs no feature and binds the way a host binds it
    auto const one = reports_for(cc::format("{}binding b:\n    t: texture_2d[float4][1]\n", granted));
    CHECK(one.contains("invalid-constant-argument"));
    CHECK(one.contains("texture_2d[float4][1] is a binding array of one element; a binding array has at least 2"));

    // a constant index names one of its elements
    auto const past = reports_for(cc::format("{}binding b:\n    t: texture_2d[float4][2]\n\n"
                                             "@compute(1) fun cs(){{b}}:\n    let n = b.t[2].load(int2(0, 0))\n",
                                             granted));
    CHECK(past.contains("2 is no index into texture_2d[float4][2], whose elements are 0 ..< 2"));

    auto const whole = reports_for(cc::format("{}binding b:\n    t: texture_2d[float4][8]\n\n"
                                              "@compute(1) fun cs(){{b}}:\n    let n = b.t\n",
                                              granted));
    CHECK(whole.contains("b.t is a binding array, read by element: `b.t[i]`"));
}
