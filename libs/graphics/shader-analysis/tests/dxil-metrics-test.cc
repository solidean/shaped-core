#include <clean-core/streams/file_stream.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shader-analysis/metrics.hh>

using namespace cc::primitive_defines;

// tiny_cs.dxil is DXC's of a loop, a branch and a sqrt; babel-data's dxil-test.cc shows its source.

TEST("shan analyze - DXC's DXIL counts its operations by kind, its blocks, its loop and its live words")
{
    auto file = cc::file_read_stream_adapter::open(nx::source_path(SHAN_DXIL_DATA_DIR "/tiny_cs.dxil"));
    REQUIRE(file.has_value());
    auto stream = file.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    auto const m = shan::analyze_binary(bytes.value());
    REQUIRE(m.has_value()).dump("error", m.has_error() ? m.error().to_string() : cc::string());
    REQUIRE(m.value().functions.size() == 1);
    auto const& f = m.value().functions[0];
    CHECK(f.name == "main");
    CHECK(f.blocks == 7);
    CHECK(f.loops == 1);
    CHECK(f.instructions == 31);
    CHECK(f.count_of(shan::op_kind::phi) == 4);
    CHECK(f.count_of(shan::op_kind::load) == 1);
    CHECK(f.count_of(shan::op_kind::store) == 1);
    CHECK(f.count_of(shan::op_kind::extended) == 1);         // the sqrt
    CHECK(f.count_of(shan::op_kind::float_arithmetic) == 3); // the loop's multiply and two adds
    CHECK(f.count_of(shan::op_kind::int_arithmetic) == 1);   // its counter
    CHECK(f.count_of(shan::op_kind::address) == 4);          // two handles made, two annotated
    CHECK(f.count_of(shan::op_kind::composite) == 3);
    CHECK(f.count_of(shan::op_kind::control) == 7);
    CHECK(f.max_live_words > 0);
    CHECK(f.critical_path >= shan::latency_of(shan::op_kind::load));
}
