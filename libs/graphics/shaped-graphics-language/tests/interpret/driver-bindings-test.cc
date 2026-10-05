#include "driver-test-support.hh"

using namespace sgl_test;

namespace
{
constexpr cc::string_view k_inputs = "binding inputs:\n"
                                     "    scale: float\n"
                                     "    offset: float3\n"
                                     "    count: int\n"
                                     "    values: buffer[float]\n"
                                     "    sums: mut buffer[float]\n";
} // namespace

TEST("sgl driver bindings - a test reads the values and buffers its driver binds, and its stores come back")
{
    auto const sums = floats(0.0f, 5.0f);
    u32 const three[] = {3};
    auto const bindings = sgl::check::driver_bindings{.groups = {{
                                                          .name = "inputs",
                                                          .members = {
                                                              {.name = "scale", .bytes = floats(2.0f)},
                                                              {.name = "offset", .bytes = floats(0.5f, -1.5f, 4.0f)},
                                                              {.name = "count", .bytes = bytes_of(three)},
                                                              {.name = "values", .bytes = floats(1.0f, 2.0f, 3.0f)},
                                                              {.name = "sums", .mutable_bytes = sums},
                                                          },
                                                      }}};
    auto source = cc::string(k_inputs);
    source += "test {inputs}:\n"
              "    inputs.scale == 2.0\n"
              "    inputs.offset.x == 0.5 and inputs.offset.y == -1.5 and inputs.offset.z == 4.0\n"
              "    let mut total = 0.0\n"
              "    for i in 0 ..< inputs.count:\n"
              "        total += inputs.values[i] * inputs.scale\n"
              "    inputs.sums[0] = total\n"
              "    total == 12.0\n";
    CHECK(driven_failures_of(source, bindings) == "");
    // the run stored to the first element alone, and the driver sees the whole buffer as the run left it
    CHECK(float_at(sums, 0) == 12.0f);
    CHECK(float_at(sums, 1) == 5.0f);
}

TEST("sgl driver bindings - bytes are words a test loads and stores at byte offsets")
{
    u32 const words[] = {0x3f800000u, 7u, 8u, 9u};
    auto out = floats(0.0f, 0.0f);
    auto const bindings = sgl::check::driver_bindings{
        .groups = {{.name = "raw",
                    .members = {{.name = "src", .bytes = bytes_of(words)}, {.name = "dst", .mutable_bytes = out}}}}};
    auto const source = cc::string("binding raw:\n"
                                   "    src: bytes\n"
                                   "    dst: mut bytes\n"
                                   "test {raw}:\n"
                                   "    float.from_bits(raw.src.load(0u)) == 1.0\n"
                                   "    let three = raw.src.load3(4u)\n"
                                   "    three.x == 7u and three.y == 8u and three.z == 9u\n"
                                   "    raw.dst.store(0u, uint2(three.z, three.x))\n"
                                   "    raw.dst.load(4u) == 7u\n");
    CHECK(driven_failures_of(source, bindings) == "");
    CHECK(cc::bit_cast<u32>(float_at(out, 0)) == 9u);
    CHECK(cc::bit_cast<u32>(float_at(out, 1)) == 7u);

    // EVAL-97: a word that straddles two, or one past the end, has no behaviour
    auto const misaligned = cc::string("binding raw:\n    src: bytes\n    dst: mut bytes\n"
                                       "@expect(.fail)\n"
                                       "test {raw}:\n    raw.src.load(2u) == 0u\n");
    CHECK(driven_failures_of(misaligned, bindings).contains("the byte offset 2 is no multiple of 4"));
    auto const past = cc::string("binding raw:\n    src: bytes\n    dst: mut bytes\n"
                                 "@expect(.fail)\n"
                                 "test {raw}:\n    raw.src.load2(12u).x == 0u\n");
    CHECK(driven_failures_of(past, bindings).contains("2 words from byte 12 are out of bounds of 16 bytes"));
}

TEST("sgl driver bindings - what the driver leaves out is zero, and a buffer it leaves out is empty")
{
    auto source = cc::string(k_inputs);
    source += "test {inputs}:\n"
              "    inputs.scale == 0.0 and inputs.count == 0\n"
              "@expect(.fail)\n"
              "test {inputs}:\n"
              "    inputs.values[0] == 0.0\n";
    // EVAL-90: an empty buffer holds no element, so the second run has no behaviour past its read
    CHECK(driven_failures_of(source, {})
          == "test-failed user:[test] the run has no behaviour past this: the index 0 is out of bounds of a buffer of "
             "0 elements\n");
}

TEST("sgl driver bindings - a member the binding lacks, or bytes of no whole value, are the driver's mistake")
{
    auto source = cc::string(k_inputs);
    source += "test {inputs}:\n    inputs.scale == 0.0\n";
    auto const unknown = sgl::check::driver_bindings{
        .groups = {{.name = "inputs", .members = {{.name = "exposure", .bytes = floats(1.0f)}}}}};
    CHECK(driven_failures_of(source, unknown).contains("the binding inputs has no member exposure"));
    auto const short_value = sgl::check::driver_bindings{
        .groups = {{.name = "inputs", .members = {{.name = "offset", .bytes = floats(1.0f, 2.0f)}}}}};
    CHECK(driven_failures_of(source, short_value).contains("inputs.offset is 8 bytes, and its value is 12"));
    auto const ragged = sgl::check::driver_bindings{
        .groups
        = {{.name = "inputs", .members = {{.name = "values", .bytes = floats(1.0f).subdata({.offset = 0, .size = 2})}}}}};
    CHECK(driven_failures_of(source, ragged)
              .contains("inputs.values is 2 bytes, which is no whole number of 4-byte elements"));
    // a group the test does not list is never read, so one set of bindings serves every test
    auto const other = sgl::check::driver_bindings{
        .groups = {{.name = "unlisted", .members = {{.name = "anything", .bytes = floats(1.0f)}}}}};
    CHECK(driven_failures_of(source, other) == "");
}

TEST("sgl driver bindings - a test lists values, buffers and acceleration structures, and no texture yet")
{
    // CHK-333: a callee's binding the test lists is the test's own, and no capture of the function it stands in
    CHECK(reports_for("binding frame:\n    exposure: float\n"
                      "fun exposed(){frame} -> float:\n"
                      "    test {frame}:\n"
                      "        frame.exposure == 0.0\n"
                      "    return frame.exposure\n"
                      "test {frame}:\n    exposed() == 0.0\n")
          == "");
    CHECK(reports_for("binding material:\n    albedo: texture_2d[float4]\n    tint: float\n"
                      "test {material}:\n    material.tint == 0.0\n")
          == "unsupported-yet user:[test] a test that lists material, whose albedo is a texture, an image or a "
             "sampler\n");
    CHECK(reports_for("binding frame:\n    exposure: float\nfun exposed(){frame} -> float => frame.exposure\n"
                      "test {frame, exposed}:\n    true\n")
          == "wrong-kind-of-name user:[exposed] exposed is no binding\n");
    // listing one binding says nothing of another
    CHECK(reports_for("binding a:\n    x: float\nbinding b:\n    y: float\ntest {a}:\n    b.y == 0.0\n")
          == "binding-not-listed user:[b] b is a binding, and the test does not list it\n");
}
