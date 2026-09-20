#include "check-test-support.hh"

using namespace sgl_test;

// The binding model of libs/graphics/shaped-graphics-language/docs/spec/bindings.md, as far as the compiler carries it.
// Everything here parses and is then reported by name: the shape is decided, and none of it is built yet.

namespace
{
/// A binding and an entry point that lists it, since a binding nobody demands is never checked.
cc::string listing(cc::string_view members)
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
                      "    return {{color = float4(1.0, 1.0, 1.0, 1.0)}}\n",
                      members);
}
} // namespace

TEST("sgl check - a resource type is reported by the feature it needs")
{
    auto const reports = listing("    scale: float\n"
                                 "    src: buffer[float]\n"
                                 "    dst: mut buffer[float]\n"
                                 "    raw: bytes\n");

    auto const found = reports_for(reports);

    // A plain member is a constant of the group's own buffer, so it needs nothing and reports nothing.
    CHECK(!found.contains("scale"));

    CHECK(found.contains("unsupported-yet user:[buffer[float]] type arguments"));
    CHECK(found.contains("unsupported-yet user:[mut buffer[float]] a `mut` resource"));
    // `bytes` has no declaration behind it yet, so it is an unknown name rather than an unbuilt feature.
    CHECK(found.contains("unknown-name user:[bytes] bytes"));
}

TEST("sgl check - mut and out report what they qualify, not the type under them")
{
    CHECK(reports_for(listing("    x: mut buffer[float]\n")).contains("a `mut` resource"));
    CHECK(reports_for(listing("    x: out texture2d[rgba8unorm]\n")).contains("an `out` resource"));
}
