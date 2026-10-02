#include "check-test-support.hh"

#include <nexus/test.hh>

using namespace sgl_test;

// Image subscripts (CHK-367), coherent members (CHK-368) and atomic images (CHK-372, CHK-373).
// A test lists no binding that holds an image, and WebGPU has neither coherence nor image atomics, so the corpus can say
// none of these: each case is an entry point, which a corpus file would have to write for WGSL too.

namespace
{
/// Every member kind a case below needs, behind both features.
constexpr auto k_bindings = cc::string_view(R"(require device_coherence
require image_atomics
require readwrite_image_formats

binding canvas:
    weights: mut image_2d[.r32_float]
    target: out image_2d[.rgba8_unorm]
    source: image_2d[.rgba8_unorm]
    layers: mut image_2d_array[.r32_float]
    swatch: mut image_2d[.rgba32_float]
    photo: texture_2d[float4]

binding prepare:
    @atomic depth: mut image_2d[.r32_uint]
    @atomic offsets: mut image_2d_array[.r32_sint]

)");

/// `body` as the body of a compute entry point listing both bindings, behind them.
cc::string reports_for_body(cc::string_view body)
{
    return reports_for(cc::format("{}@compute(8, 8) fun cs(@thread_id id: int3){{canvas, prepare}}:\n"
                                  "    let xy = id.xy\n"
                                  "{}",
                                  k_bindings, body));
}

/// `members` as the members of one binding a compute entry point lists, with the file's `require` lines first.
cc::string reports_for_members(cc::string_view members,
                               cc::string_view require = "require device_coherence\n"
                                                         "require image_atomics\n")
{
    return reports_for(cc::format("{}\n"
                                  "binding set:\n"
                                  "{}"
                                  "\n"
                                  "@compute(1) fun cs(){{set}}:\n"
                                  "    let unused = 1\n",
                                  require, members));
}
} // namespace

TEST("sgl check - an image's subscript reads where the image may be read and writes where it may be written")
{
    // CHK-367: the `load` and the `store` it stands for, through a resource parameter as through the member
    CHECK(reports_for_body("    canvas.target[xy] = canvas.source[xy]\n"
                           "    canvas.weights[xy] += canvas.source[xy].x\n"
                           "    canvas.layers[xy, layer = 1] = canvas.weights[xy]\n"
                           "    canvas.swatch[xy] = canvas.swatch[xy].wzyx\n")
          == "");
    CHECK(reports_for(cc::string(k_bindings)
                      + "fun bump(img: mut image_2d[.r32_float], xy: int2):\n"
                        "    img[xy] -= 1.0\n"
                        "\n"
                        "@compute(8, 8) fun cs(@thread_id id: int3){canvas}:\n"
                        "    bump(canvas.weights, id.xy)\n")
          == "");

    // an `out` image has no `load`, and one without an access word no `store`
    CHECK(reports_for_body("    let c = canvas.target[xy]\n").contains("no-matching-overload"));
    CHECK(reports_for_body("    canvas.target[xy] += float4(1.0, 1.0, 1.0, 1.0)\n").contains("no-matching-overload"));
    CHECK(reports_for_body("    canvas.source[xy] = float4(1.0, 1.0, 1.0, 1.0)\n").contains("no-matching-overload"));
    // the value is the texel's type, which a literal converts to as it does for any place
    CHECK(reports_for_body("    canvas.weights[xy] = 1\n") == "");
    CHECK(reports_for_body("    canvas.weights[xy] = float2(1.0, 1.0)\n").contains("type-mismatch"));
    // a layer is named, as `load` names it
    CHECK(reports_for_body("    let w = canvas.layers[xy, 1]\n").contains("no-matching-overload"));
    CHECK(reports_for_body("    let w = canvas.weights[xy.x]\n").contains("no-matching-overload"));
}

TEST("sgl check - a texel is stored whole, and a sampled texture has no subscript yet")
{
    // CHK-367: a part of a texel is no place, and no call writes through one
    CHECK(reports_for_body("    canvas.swatch[xy].x = 1.0\n")
              .contains("not-assignable user:[canvas.swatch[xy].x] a texel is stored whole"));
    CHECK(reports_for_body("    canvas.swatch[xy].xy = float2(1.0, 1.0)\n").contains("not-assignable"));
    CHECK(reports_for(cc::string(k_bindings)
                      + "fun clear(c: mut float4):\n"
                        "    c = float4(0.0, 0.0, 0.0, 0.0)\n"
                        "\n"
                        "@compute(8, 8) fun cs(@thread_id id: int3){canvas}:\n"
                        "    clear(mut canvas.swatch[id.xy])\n")
              .contains("not-assignable"));
    CHECK(reports_for_body("    let c = canvas.photo[xy]\n").contains("unsupported-yet"));
}

TEST("sgl check - @coherent stands on a mut buffer or a mut image, and needs device_coherence")
{
    // CHK-368, on a binding array's elements as on one resource
    CHECK(reports_for_members("    @coherent a: mut buffer[float]\n"
                              "    @coherent b: mut image_2d[.r32_float]\n"
                              "    @coherent c: mut buffer[atomic[uint]]\n")
          == "");
    CHECK(reports_for_members("    @coherent a: mut buffer[float]\n", "")
              .contains("needs-feature user:[coherent] a @coherent member needs device_coherence"));
    for (auto const member : {"    @coherent a: buffer[float]\n", "    @coherent a: out image_2d[.r32_float]\n",
                              "    @coherent a: texture_2d[float4]\n", "    @coherent a: float\n",
                              "    @coherent a: image_2d[.r32_float]\n"})
        CHECK(reports_for_members(member).contains("wrong-kind-of-name user:[coherent]")).dump("member", member);
}

TEST("sgl check - @atomic stands on a mut image of one 32-bit integer channel, and needs image_atomics")
{
    // CHK-372
    CHECK(reports_for_members("    @atomic a: mut image_2d[.r32_uint]\n"
                              "    @atomic b: mut image_3d[.r32_sint]\n"
                              "    @atomic c: mut image_1d_array[.r32_uint]\n")
          == "");
    CHECK(reports_for_members("    @atomic a: mut image_2d[.r32_uint]\n", "")
              .contains("needs-feature user:[atomic] an @atomic image needs image_atomics"));
    for (auto const member :
         {"    @atomic a: mut image_2d[.r32_float]\n", "    @atomic a: image_2d[.r32_uint]\n",
          "    @atomic a: out image_2d[.r32_uint]\n", "    @atomic a: mut buffer[uint]\n", "    @atomic a: uint\n"})
        CHECK(reports_for_members(member).contains("wrong-kind-of-name user:[atomic]")).dump("member", member);
}

TEST("sgl check - an atomic image's texel is an atomic, reached by a subscript and updated by its methods")
{
    // CHK-373: each update gives the value before, of the texel's own type
    CHECK(reports_for_body("    let a: uint = prepare.depth[xy / 2].max(7u)\n"
                           "    let b: int = prepare.offsets[xy, layer = 1].add(-2)\n"
                           "    let c: uint = prepare.depth[xy].load()\n"
                           "    prepare.depth[xy].store(a + c)\n"
                           "    let d: uint = prepare.depth[xy].exchange(prepare.depth.size().x as uint)\n")
          == "");
    CHECK(reports_for_body("    let a: int = prepare.depth[xy].max(7u)\n").contains("type-mismatch"));

    // a plain access would race an update, so the texel stands only as its method's receiver (CHK-297)
    for (auto const body :
         {"    let t = prepare.depth[xy]\n", "    prepare.depth[xy] = 1u\n", "    prepare.depth[xy] += 1u\n",
          "    let t = prepare.depth.load(xy)\n", "    prepare.depth.store(xy, 1u)\n"})
        CHECK(reports_for_body(body).contains("wrong-kind-of-name")).dump("body", body);
    // the coordinate is what the plain image's `load` takes
    CHECK(reports_for_body("    let a = prepare.offsets[xy, 1].add(1)\n").contains("no-matching-overload"));

    // a function of the program takes no atomic image: a parameter type has no @atomic, and a plain access would race
    CHECK(reports_for(cc::string(k_bindings)
                      + "fun raise(img: mut image_2d[.r32_uint], xy: int2):\n"
                        "    img[xy] = 1u\n"
                        "\n"
                        "@compute(8, 8) fun cs(@thread_id id: int3){prepare}:\n"
                        "    raise(prepare.depth, id.xy)\n")
              .contains("no-matching-overload"));
}

TEST("sgl check - an atomic image's update stands in a pixel or compute stage, and gives a non-uniform value")
{
    // CHK-296: every atomic's stages
    CHECK(reports_for(cc::string(k_bindings)
                      + "struct vout:\n"
                        "    @position p: hpos4\n"
                        "\n"
                        "@vertex fun vs(){prepare} -> vout:\n"
                        "    let d = prepare.depth[int2(0, 0)].max(1u)\n"
                        "    return { p = hpos4(d as float, 0.0, 0.0, 1.0) }\n")
              .contains("stage-not-allowed"));
    // CHK-283: what another invocation did first is what an update gives
    CHECK(reports_for_body("    if prepare.depth[xy].max(1u) > 0u => workgroup_barrier()\n")
              .contains("non-uniform-control-flow"));
}

TEST("sgl footprint - a subscript is the load or the store it stands for, and a texel's update reads and writes")
{
    CHECK(reports_for(cc::string(k_bindings)
                      + "@expect(footprint = \"canvas.weights: read write, canvas.target: write, canvas.source: read, "
                        "prepare.depth: read write\")\n"
                        "@compute(8, 8) fun cs(@thread_id id: int3){canvas, prepare}:\n"
                        "    canvas.target[id.xy] = canvas.source[id.xy]\n"
                        "    canvas.weights[id.xy] *= 2.0\n"
                        "    let before = prepare.depth[id.xy].max(1u)\n")
          == "");
}
