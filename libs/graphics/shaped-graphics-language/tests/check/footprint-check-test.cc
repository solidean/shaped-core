#include "check-test-support.hh"

#include <nexus/test.hh>

using namespace sgl_test;

// What an entry point's code does to each binding it lists, pinned with `@expect(footprint = "...")`.
// A C++ test rather than a corpus file only because MSL takes no binding group yet, and a corpus file must write every
// entry point for every target; the pins themselves are SGL.

namespace
{
/// The bindings every case below lists.
constexpr auto k_bindings = cc::string_view(R"(binding work:
    scale: float
    values: mut buffer[float]
    source: buffer[float]
    unused: buffer[float]

binding post:
    src: texture_2d[float4]
    dst: out image_2d[.rgba8_unorm]
    acc: mut image_2d[.r32_float]
    sampler bilinear:
        filter = .linear
        address = .clamp_edge

)");

cc::string reports_for_entry(cc::string_view entry)
{
    return reports_for(cc::string(k_bindings) + cc::string(entry));
}
} // namespace

TEST("sgl footprint - a member the code never names is untouched, and the constant block is one slot")
{
    CHECK(reports_for_entry(R"(@expect(footprint = "work: read, work.source: read, work.values: write")
@compute(64) fun main(@thread_id id: int3){work}:
    work.values[id.x] = work.source[id.x] * work.scale
)") == "");
}

TEST("sgl footprint - a mut buffer that is only loaded is read, and one that is added to is read and written")
{
    CHECK(reports_for_entry(R"(@expect(footprint = "work.values: read")
@compute(64) fun only_loads(@thread_id id: int3){work}:
    let x = work.values[id.x]
)") == "");
    CHECK(reports_for_entry(R"(@expect(footprint = "work.values: read write")
@compute(64) fun adds(@thread_id id: int3){work}:
    work.values[id.x] += 1.0
)") == "");
}

TEST("sgl footprint - a use in a called function counts, and so does one in a branch that may not run")
{
    CHECK(reports_for_entry(R"(fun load(i: int){work} -> float => work.source[i]

@expect(footprint = "work.source: read, work.values: write")
@compute(64) fun main(@thread_id id: int3){work}:
    if id.x > 3:
        work.values[id.x] = load(id.x)
)") == "");
}

TEST("sgl footprint - a builtin's parameter says how an image is used, and a sampler is never a slot")
{
    CHECK(reports_for_entry(R"(@expect(footprint = "post.src: read, post.dst: write, post.acc: read write")
@compute(8, 8) fun main(@thread_id id: int3){post}:
    let xy = int2(id.x, id.y)
    let c = post.src.sample(float2(0.5, 0.5), post.bilinear, level = 0.0)
    post.dst.store(xy, c)
    post.acc.store(xy, post.acc.load(xy) + c.x)
)") == "");
}

TEST("sgl footprint - a use under deep nesting still counts")
{
    // The footprint has no nesting limit of its own, so statements nested inside statements add nothing to what an
    // expression under them may hold: 100 of each is well inside what the emitter prints.
    auto source = cc::string(R"(@expect(footprint = "work.source: read, work.values: write")
@compute(64) fun main(@thread_id id: int3){work}:
)");
    auto indent = cc::string("    ");
    for (auto i = 0; i < 100; ++i)
    {
        source += indent + "if id.x >= 0:\n";
        indent += "    ";
    }
    source += indent + "work.values[id.x] = work.source[id.x]";
    for (auto i = 0; i < 100; ++i)
        source += " + 1.0";
    source += "\n";
    CHECK(reports_for_entry(source) == "");
}

TEST("sgl footprint - the order a pin names its slots in does not matter")
{
    CHECK(reports_for_entry(R"(@expect(footprint = "work.values: write,work.source:read,  work:read")
@compute(64) fun main(@thread_id id: int3){work}:
    work.values[id.x] = work.source[id.x] * work.scale
)") == "");
}

TEST("sgl footprint - a pin the code disagrees with is unmet, and says what the footprint is")
{
    auto const reports = reports_for_entry(R"(@expect(footprint = "work.values: read")
@compute(64) fun main(@thread_id id: int3){work}:
    work.values[id.x] = 1.0
)");
    CHECK(reports.contains("unmet-expectation"));
    CHECK(reports.contains("the footprint of 'main' is \"work.values: write\""));
}

TEST("sgl footprint - only an entry point has a footprint to pin, and it pins nothing else")
{
    CHECK(reports_for_entry(R"(@expect(footprint = "work: read")
fun helper(){work} -> float => work.scale
)")
              .contains("only an entry point has a footprint to pin"));
    CHECK(reports_for_entry(R"(@expect(error = "x")
@compute(64) fun main(@thread_id id: int3){work}:
    work.values[id.x] = 1.0
)")
              .contains("invalid-attribute-arguments"));
    CHECK(reports_for_entry(R"(@expect()
@compute(64) fun main(@thread_id id: int3){work}:
    work.values[id.x] = 1.0
)")
              .contains("invalid-attribute-arguments"));
}
