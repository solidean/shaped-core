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

TEST("sgl check - a buffer's host name is its own, module-wide (CHK-172)")
{
    // `_` may stand anywhere in a name, so `<binding>_<member>` alone does not keep two buffers apart.
    auto const two_groups = cc::string("binding a:\n    b_c: buffer[float]\n\n") + listing("    unused: buffer[float]\n");
    CHECK(reports_for(two_groups) == "");

    auto const clash = cc::string("binding work_a:\n    b: buffer[float]\n\n") + listing("    a_b: buffer[float]\n");
    CHECK(reports_for(clash).contains("duplicate-reflected-name"));
    CHECK(reports_for(clash).contains("'work_a_b'"));

    // A module-level declaration's name is taken too, since the host name is a global in every target.
    auto const with_function = cc::string("fun work_src() -> float => 1.0\n\n") + listing("    src: buffer[float]\n");
    CHECK(reports_for(with_function).contains("a module-level declaration"));

    // A plain member is no buffer and has no host name of its own.
    CHECK(reports_for(cc::string("fun work_scale() -> float => 1.0\n\n") + listing("    scale: float\n")) == "");
}
