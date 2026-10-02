#include "check-test-support.hh"

#include <clean-core/string/format.hh>

using namespace sgl_test;

// CHK-201 and CHK-258 to CHK-265: `require` grants a feature, and an entry point declares what it needs of a device.

namespace
{
constexpr cc::string_view k_edges = "struct pixel_input:\n"
                                    "    @position position: hpos4\n"
                                    "\n"
                                    "@pixel struct target:\n"
                                    "    color: float4\n"
                                    "\n";

/// A binding of `members` and a pixel entry point that lists it, behind `head` at file scope.
cc::string listing(cc::string_view head, cc::string_view members, cc::string_view body = "")
{
    return cc::format("{}binding work:\n"
                      "{}"
                      "\n"
                      "{}"
                      "@pixel fun main_ps(p: pixel_input){{work}} -> target:\n"
                      "{}"
                      "    return {{color = float4(1.0, 1.0, 1.0, 1.0)}}\n",
                      head, members, k_edges, body);
}

/// `user` behind the library's prelude and one more file, `library`, which stands for a module the user file uses.
cc::string reports_with_library(cc::string_view library, cc::string_view user)
{
    auto sources = cc::vector<cc::string_view>();
    for (auto const& p : sgl::prelude_files())
        sources.push_back(p.source);
    sources.push_back(library);
    sources.push_back(user);
    return reports_of(check_files(sources));
}

/// The pixel entry point of the user file, listing `lib` from the library file.
cc::string lib_user(cc::string_view body = "")
{
    return cc::format("{}@pixel fun main_ps(p: pixel_input){{lib}} -> target:\n"
                      "{}"
                      "    return {{color = float4(1.0, 1.0, 1.0, 1.0)}}\n",
                      k_edges, body);
}
} // namespace

TEST("sgl check - a form some backend lacks is refused unless a require grants its feature")
{
    constexpr auto r8 = "    a: out image_2d[.r8_unorm]\n";
    CHECK(reports_for(listing("", r8))
          == "needs-feature user:[image_2d[.r8_unorm]] an image of .r8_unorm needs extended_image_formats, "
             "which `require extended_image_formats` grants\n");

    // CHK-259 and CHK-260: the file's `require` or the binding's own.
    CHECK(reports_for(listing("require extended_image_formats\n\n", r8)) == "");
    CHECK(reports_for(listing("", cc::format("    require extended_image_formats\n{}", r8))) == "");

    // One `require` may name several; each form asks for its own.
    constexpr auto both = "    a: mut image_2d[.r8_unorm]\n";
    CHECK(reports_for(listing("require extended_image_formats\n\n", both)).contains("readwrite_image_formats"));
    CHECK(reports_for(listing("require extended_image_formats, readwrite_image_formats\n\n", both)) == "");

    constexpr auto ms = "    a: texture_2d_ms_array[float4]\n";
    CHECK(reports_for(listing("", ms)).contains("needs multisampled_array_textures"));
    CHECK(reports_for(listing("require multisampled_array_textures\n\n", ms)) == "");
}

TEST("sgl check - a require names a feature a shader can use, as sg names it")
{
    // CHK-258: an sg feature the host alone asks about is no name here.
    CHECK(reports_for("require timestamp_query\n")
          == "unknown-feature user:[timestamp_query] timestamp_query; a shader may require binding_arrays, "
             "extended_image_formats, readwrite_image_formats, multisampled_array_textures, ray_query, "
             "raytracing_pipeline, primitive_index, "
             "sample_rate_shading, geometry_shader, tessellation_shader, shader_f16, shader_int16, subgroups, "
             "device_coherence, image_atomics\n");
    CHECK(reports_for("require raytracing\n").contains("unknown-feature user:[raytracing]"));
    CHECK(reports_for("require ray_query, binding_arrays\n") == "");

    // The 16-bit types, the subgroup operations, coherent memory and image atomics are names too.
    CHECK(reports_for("require shader_f16, shader_int16, subgroups, device_coherence, image_atomics\n") == "");
}

TEST("sgl check - a require in a body that nothing needs is unused, and one of a file or a binding never is")
{
    // CHK-265: a file's and a binding's `require` each declare an intent, whether anything uses the feature or not.
    CHECK(reports_for("require ray_query\n") == "");
    CHECK(reports_for(listing("", "    require ray_query\n    a: float\n")) == "");
    CHECK(reports_for(listing("", "    require extended_image_formats\n"
                                  "    require extended_image_formats\n"
                                  "    a: out image_2d[.r8_unorm]\n"))
          == "");

    // What a binding declares is what its listers need, so the floor holds it even where no member uses it.
    auto const declared_only = check_sources(read_prelude(), listing("", "    require ray_query\n    a: float\n"));
    REQUIRE(declared_only.module.entry_points.size() == 1);
    CHECK(declared_only.module.entry_points[0].features == sgl::check::feature_set(sgl::check::feature::ray_query));

    // A helper's `require` that no call its body reaches needs is unused.
    constexpr auto helper = "fun helper() -> float:\n"
                            "    require ray_query\n"
                            "    return 1.0\n";
    CHECK(reports_for(helper) == "unused-require user:[ray_query] nothing in helper needs ray_query of it\n");

    // An entry point's body that says again what its file already declares declares nothing.
    CHECK(reports_for(listing("require extended_image_formats\n\n", "    a: out image_2d[.r8_unorm]\n",
                              "    require extended_image_formats\n"))
              .contains("unused-require user:[extended_image_formats] nothing in main_ps needs"));
}

TEST("sgl check - a require stands among a body's own lines, and in no struct")
{
    // CHK-262: a block-scoped grant is not carried yet.
    CHECK(reports_for("fun f(c: bool) -> float:\n"
                      "    if c:\n"
                      "        require ray_query\n"
                      "    return 1.0\n")
              .contains("unsupported-yet"));
    // The AST pass reports one in a struct (AST-146), and this pass says nothing more of it.
    CHECK(reports_for("struct s:\n    require ray_query\n    x: float\n") == "");
}

TEST("sgl check - an entry point declares what it needs, by its file, a binding it lists, or its own body")
{
    // The library grants its binding the feature, which says nothing about a file that uses the binding.
    constexpr auto library = "require extended_image_formats\n"
                             "\n"
                             "binding lib:\n"
                             "    a: out image_2d[.r8_unorm]\n";

    // CHK-264: named at the entry point, with the binding it needs the feature for.
    CHECK(reports_with_library(library, lib_user())
          == "feature-not-declared user:[main_ps] main_ps needs extended_image_formats, which neither its file, "
             "a binding it lists nor its body requires\n"
             "  note prelude:[lib] lib needs extended_image_formats\n");

    // CHK-262: each of the three places declares it.
    CHECK(reports_with_library(library, lib_user("    require extended_image_formats\n")) == "");
    CHECK(reports_with_library(library, cc::format("require extended_image_formats\n\n{}", lib_user())) == "");
    constexpr auto requiring = "binding lib:\n"
                               "    require extended_image_formats\n"
                               "    a: out image_2d[.r8_unorm]\n";
    CHECK(reports_with_library(requiring, lib_user()) == "");
}

TEST("sgl check - an entry point needs what it uses, and a feature its file permits is no floor")
{
    // CHK-263: two entry points of one file under one `require`, only one of which lists what needs it.
    auto const checked
        = check_sources(read_prelude(), cc::format("require extended_image_formats, ray_query\n"
                                                   "\n"
                                                   "binding narrow:\n"
                                                   "    a: out image_2d[.r8_unorm]\n"
                                                   "\n"
                                                   "binding wide:\n"
                                                   "    b: out image_2d[.rgba8_unorm]\n"
                                                   "\n"
                                                   "{}"
                                                   "@pixel fun narrow_ps(p: pixel_input){{narrow}} -> target:\n"
                                                   "    return {{color = float4(1.0, 1.0, 1.0, 1.0)}}\n"
                                                   "\n"
                                                   "@pixel fun wide_ps(p: pixel_input){{wide}} -> target:\n"
                                                   "    return {{color = float4(1.0, 1.0, 1.0, 1.0)}}\n",
                                                   k_edges));
    CHECK(reports_of(checked) == "");
    REQUIRE(checked.module.entry_points.size() == 2);
    for (auto const& e : checked.module.entry_points)
    {
        auto const expected = e.name == "narrow_ps"
                                ? sgl::check::feature_set(sgl::check::feature::extended_image_formats)
                                : sgl::check::feature_set();
        CHECK(e.features == expected);
    }
}

TEST("sgl check - a stage input some device lacks needs its feature, as a binding member does")
{
    // CHK-272: `@primitive_id` in the pixel stage and `@sample_index` are what vulkan gives only with a device feature
    constexpr auto edges = "struct vout:\n    @position p: hpos4\n\n@pixel struct target:\n    c: float4\n\n";
    CHECK(reports_for(cc::string(edges)
                      + "@pixel fun ps(v: vout, @primitive_id id: int) -> target:\n"
                        "    return { c = float4(id as float, 0.0, 0.0, 1.0) }\n")
              .contains("feature-not-declared user:[ps] ps needs primitive_index"));
    CHECK(reports_for(cc::string("require primitive_index, sample_rate_shading\n") + edges
                      + "@pixel fun ps(v: vout, @primitive_id id: int, @sample_index s: int) -> target:\n"
                        "    return { c = float4(id as float, s as float, 0.0, 1.0) }\n")
          == "");
    // one every device has needs nothing
    CHECK(reports_for(cc::string(edges)
                      + "@pixel fun ps(v: vout, @is_front_facing f: bool) -> target:\n"
                        "    return { c = float4(1.0, 1.0, 1.0, 1.0) }\n")
          == "");
}

TEST("sgl check - a 16-bit value needs its feature wherever an entry point or a binding holds one")
{
    // CHK-347: a value in the body, which the entry point declares like a call's feature
    auto const body = cc::string("@compute(64) fun cs(@thread_id id: int3):\n"
                                 "    let h = (id.x as float) as half\n"
                                 "    let s = h * 2.0\n");
    CHECK(reports_for(body)
          == "feature-not-declared user:[cs] cs needs shader_f16, which neither its file, a binding it lists nor its "
             "body requires\n"
             "  note user:[(id.x as float) as half] a value that needs it\n");
    CHECK(reports_for(cc::string("require shader_f16\n\n") + body) == "");
    CHECK(reports_for("@compute(64) fun cs(@thread_id id: int3):\n    let s = 3i16 + 1i16\n")
              .contains("cs needs shader_int16"));
    // a test runs on no device, and needs nothing
    CHECK(reports_for("test (0.5f16 * 2f16) as float == 1.0\n") == "");

    // CHK-201: a binding member holding one is a form some backend lacks
    CHECK(reports_for(listing("", "    gain: half\n"))
          == "needs-feature user:[half] a value of half needs shader_f16, which `require shader_f16` grants\n");
    CHECK(reports_for(listing("", "    counts: mut buffer[ushort2]\n")).contains("needs shader_int16"));
    CHECK(reports_for(listing("require shader_f16\n\n", "    gain: half\n")) == "");
}

TEST("sgl check - no 16-bit value crosses a stage edge, and no buffer strides by part of a word")
{
    // CHK-348: at any depth of the struct an entry point takes or returns
    CHECK(reports_for("require shader_f16\n\n"
                      "struct shade:\n"
                      "    tint: half3\n"
                      "\n"
                      "struct varyings:\n"
                      "    @position position: hpos4\n"
                      "    s: shade\n"
                      "\n"
                      "@vertex fun vs() -> varyings:\n"
                      "    return {position = hpos4(0.0, 0.0, 0.0, 1.0), s = {tint = half3(1.0)}}\n")
              .contains("unsupported-yet user:[vs] a 16-bit value crossing a stage edge, in varyings.s.tint: half3"));

    // CHK-346: a buffer strides by whole 4-byte words, which a lone half or a half3 does not fill
    CHECK(reports_for(listing("require shader_f16\n\n", "    values: buffer[half]\n"))
          == "unsupported-yet user:[buffer[half]] a buffer of half, whose element takes 2 bytes, which is no whole "
             "number of 4-byte words\n");
    CHECK(reports_for(listing("require shader_f16\n\n", "    values: buffer[half3]\n")).contains("takes 6 bytes"));
    CHECK(reports_for(listing("require shader_f16\n\n", "    values: buffer[half2]\n")) == "");
}
