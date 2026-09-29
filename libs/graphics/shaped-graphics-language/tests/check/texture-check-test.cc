#include "check-test-support.hh"

using namespace sgl_test;

// The texture methods' own rules: a default sampler (CHK-279), constant arguments (CHK-280) and what may filter a depth
// texture (CHK-281).

namespace
{
/// A binding and a pixel entry point that lists it, with `body` ahead of the return.
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
                      "    let uv = float2(0.5, 0.5)\n"
                      "{}"
                      "    return {{color = float4(1.0, 1.0, 1.0, 1.0)}}\n",
                      members, body);
}
} // namespace

TEST("sgl check - a texture's @sampler names a sampler of its own binding, which a call without one reads")
{
    constexpr auto sample = "    let c = work.t.sample(uv)\n";
    CHECK(reports_for(listing("    @sampler(s) t: texture_2d[float4]\n    s: sampler\n", sample)) == "");
    // the sampler may stand below the texture, and may be a static one of the binding
    CHECK(reports_for(listing("    @sampler(s) t: texture_2d[float4]\n    sampler s:\n        filter = .linear\n", sample))
          == "");
    // one named still wins over the default
    CHECK(reports_for(listing("    @sampler(s) t: texture_2d[float4]\n    s: sampler\n    n: sampler\n",
                              "    let c = work.t.sample(uv, work.n)\n"))
          == "");

    auto const missing = reports_for(listing("    t: texture_2d[float4]\n    s: sampler\n", sample));
    CHECK(missing.contains("missing-sampler"));
    CHECK(missing.contains("work.t names no @sampler"));

    CHECK(reports_for(listing("    @sampler(q) t: texture_2d[float4]\n    s: sampler\n", sample))
              .contains("neither the binding nor its file has a sampler q"));
    CHECK(reports_for(listing("    @sampler(u) t: texture_2d[float4]\n    u: texture_2d[float]\n", sample))
              .contains("u is no sampler"));
    CHECK(reports_for(listing("    @sampler(s) b: buffer[float]\n    s: sampler\n")).contains("only a texture is sampled"));
    CHECK(reports_for(listing("    @sampler t: texture_2d[float4]\n    s: sampler\n")).contains("@sampler takes one name"));
}

TEST("sgl check - a texture's @sampler names a file-scope sampler when its binding has no member of that name")
{
    auto const with_file = [](cc::string_view samplers, cc::string_view members, cc::string_view body)
    { return reports_for(cc::format("{}\n{}", samplers, listing(members, body))); };
    constexpr auto linear = "sampler e:\n    filter = .linear\n";
    constexpr auto compare = "sampler e:\n    compare = .less\n";
    constexpr auto sample = "    let c = work.t.sample(uv)\n";

    CHECK(with_file(linear, "    @sampler(e) t: texture_2d[float4]\n", sample) == "");
    // a member of the binding hides the file's sampler of its name, which would be the wrong kind here
    CHECK(with_file(compare, "    @sampler(e) t: texture_2d[float4]\n    e: sampler\n", sample) == "");

    CHECK(with_file(linear, "    @sampler(e) d: texture_2d_depth\n",
                    "    let c = work.d.sample_compare(uv, reference = 0.5)\n")
              .contains("sample_compare takes a comparison_sampler, and the @sampler of work.d is e"));
    CHECK(with_file(compare, "    @sampler(e) t: texture_2d[float4]\n", sample)
              .contains("sample takes a sampler, and the @sampler of work.t is e"));
    CHECK(with_file(compare, "    @sampler(e) d: texture_2d_depth\n",
                    "    let c = work.d.sample_compare(uv, reference = 0.5)\n")
          == "");

    // CHK-314: a file-scope sampler filters as a static one does, whichever way the call reaches it
    CHECK(with_file(linear, "    @unfilterable @sampler(e) t: texture_2d[float4]\n",
                    "    let c = work.t.sample(uv, level = 0.0)\n")
              .contains("work.t is @unfilterable, and e filters"));
    CHECK(with_file("sampler e:\n    filter = .nearest\n", "    @unfilterable @sampler(e) t: texture_2d[float4]\n",
                    "    let c = work.t.sample(uv, level = 0.0)\n")
          == "");

    CHECK(reports_for(listing("    @sampler(main_ps) t: texture_2d[float4]\n", sample)).contains("main_ps is no sampler"));
}

TEST("sgl check - a default sampler is the kind the call takes, and filters by the same rules as a named one")
{
    constexpr auto depth = "    @sampler(s) d: texture_2d_depth\n";
    CHECK(reports_for(listing(cc::format("{}    s: comparison_sampler\n", depth),
                              "    let c = work.d.sample_compare(uv, reference = 0.5)\n"))
          == "");
    CHECK(reports_for(listing(cc::format("{}    s: sampler\n", depth), "    let c = work.d.sample_compare(uv, "
                                                                       "reference = 0.5)\n"))
              .contains("sample_compare takes a comparison_sampler, and the @sampler of work.d is s"));

    // CHK-281: WebGPU filters a depth texture in a comparison alone, so a plain sample of one never filters
    constexpr auto plain = "    let c = work.d.sample(uv, level = 0)\n";
    CHECK(reports_for(listing(cc::format("{}    @non_filtering s: sampler\n", depth), plain)) == "");
    CHECK(reports_for(listing(cc::format("{}    s: sampler\n", depth), plain))
              .contains("work.d is a depth texture, and work.s filters"));
    // WGSL takes a depth texture's level as an integer, so a fractional one is no argument on any target
    CHECK(reports_for(listing(cc::format("{}    @non_filtering s: sampler\n", depth), "    let c = work.d.sample(uv, "
                                                                                      "level = 0.5)\n"))
          != "");

    CHECK(reports_for(listing("    @unfilterable @sampler(s) t: texture_2d[float4]\n    s: sampler\n",
                              "    let c = work.t.sample(uv, level = 0.0)\n"))
              .contains("work.t is @unfilterable, and work.s filters"));
}

TEST("sgl check - an offset, a gather's component and a compare's level are constants a target takes as written")
{
    constexpr auto members = "    @sampler(s) t: texture_2d[float4]\n    s: sampler\n"
                             "    @sampler(c) d: texture_2d_depth\n    c: comparison_sampler\n";
    auto const reports = [&](cc::string_view line) { return reports_for(listing(members, line)); };
    constexpr auto kind = "invalid-constant-argument";

    CHECK(reports("    let a = work.t.sample(uv, offset = int2(1, -8))\n") == "");
    CHECK(reports("    let a = work.t.gather(uv, component = texel_component.w, offset = int2(7, 0))\n") == "");
    CHECK(reports("    let a = work.t.gather(uv, component = texel_component.y)\n") == "");
    CHECK(reports("    let a = work.d.sample_compare(uv, reference = 0.5, level = 0.0)\n") == "");

    CHECK(reports("    let o = int2(1, 1)\n    let a = work.t.sample(uv, offset = o)\n").contains(kind));
    CHECK(reports("    let a = work.t.sample(uv, offset = int2(8, 0))\n").contains("from -8 to 7"));
    CHECK(reports("    let a = work.t.sample(uv, offset = int2(0, -9))\n").contains(kind));
    CHECK(reports("    let k = texel_component.z\n    let a = work.t.gather(uv, component = k)\n").contains(kind));
    CHECK(reports("    let a = work.d.sample_compare(uv, reference = 0.5, level = 1.0)\n")
              .contains("a comparison samples level 0.0 alone"));
}
