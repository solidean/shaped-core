#include "check-test-support.hh"

using namespace sgl_test;

// The binding model of libs/graphics/shaped-graphics-language/docs/spec/bindings.md, as far as the compiler carries it.
// A buffer is built; every other resource parses and is reported by the feature it needs.

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

TEST("sgl check - a resource that is not a buffer is reported by the feature it needs")
{
    CHECK(reports_for(listing("    raw: bytes\n")).contains("unknown-name"));
    CHECK(reports_for(listing("    params: constants[dispatch_params]\n")).contains("type arguments"));
    CHECK(reports_for(listing("    result: out texture2d[rgba8unorm]\n")).contains("an `out` resource"));

    // `mut` says a resource may be written, so it says nothing about a value.
    CHECK(reports_for(listing("    wrong: mut float\n")).contains("only a resource may be `mut`"));
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
