#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/test_source.hh>
#include <shaped-graphics-language/emit/emit.hh>

using namespace cc::primitive_defines;

using sgl::emit::target;

// CHK-522: a struct SGL ships may stand in each target's own order, which a compile writes as its branch's struct.

namespace
{
constexpr cc::string_view k_writer
    = "use accel\n"
      "\n"
      "binding scene:\n"
      "    blases: buffer[accel.blas_ref]\n"
      "    writes: mut buffer[accel.ptlas_write]\n"
      "\n"
      "@compute(64) fun main_cs(@thread_id id: int3){scene}:\n"
      "    let mut w = accel.ptlas_write(index = id.x as uint, blas = scene.blases[id.x])\n"
      "    w.flags = accel.force_opaque | accel.explicit_aabb()\n"
      "    w.aabb[3] = 1.0\n"
      "    w.instance_id = 7u\n"
      "    scene.writes[id.x] = w\n";

cc::result<sgl::emitted_source, cc::string> compiled(cc::string_view source, target t)
{
    return sgl::compile_to_text({.source = source, .entry_point = "main_cs", .target = t});
}

cc::string reports_of(cc::string_view source)
{
    return sgl::test_source(source, "<target-layout>").errors;
}

/// Where `field` stands in `text`, -1 where it does not.
isize at(sgl::emitted_source const& s, cc::string_view field)
{
    return cc::string_view(s.text).find(field);
}
} // namespace

TEST("sgl target layout - a compile writes the struct in the order of the branch its target takes")
{
    auto const dx12 = compiled(k_writer, target::hlsl_dx12);
    auto const vulkan = compiled(k_writer, target::hlsl_vulkan);
    REQUIRE(dx12.has_value());
    REQUIRE(vulkan.has_value());
    // D3D12 places the BLAS before the instance's index and the box last, and Vulkan the box second and the BLAS last
    CHECK(at(dx12.value(), " blas;") < at(dx12.value(), " index;"));
    CHECK(at(dx12.value(), " index;") < at(dx12.value(), " aabb[6];"));
    CHECK(at(vulkan.value(), " aabb[6];") < at(vulkan.value(), " instance_id;"));
    CHECK(at(vulkan.value(), " partition;") < at(vulkan.value(), " blas;"));
    // the one flag the two disagree on
    auto const is_dx12_flag = dx12.value().text.contains("64u") || dx12.value().text.contains("0x40");
    auto const is_vulkan_flag = vulkan.value().text.contains("16u") || vulkan.value().text.contains("0x10");
    CHECK(is_dx12_flag);
    CHECK(is_vulkan_flag);
}

TEST("sgl target layout - a compile whose target no branch names reports the struct where it is used")
{
    auto const wgsl = compiled(k_writer, target::wgsl);
    REQUIRE(wgsl.has_error());
    CHECK(wgsl.error().contains("ptlas_write has no branch this compile takes"));
}

TEST("sgl target layout - it stands as a buffer's element and a value alone")
{
    // the check judges it by its first branch, for every target
    CHECK(reports_of(k_writer) == "");
    constexpr auto prefix = "use accel\n"
                            "\n";
    CHECK(reports_of(cc::format("{}struct holder:\n"
                                "    w: accel.ptlas_write\n"
                                "\n"
                                "fun f() -> uint:\n"
                                "    let mut h: holder\n"
                                "    return 0u\n",
                                prefix))
              .contains("a struct field holding a ptlas_write"));
    CHECK(reports_of(cc::format("{}fun f() -> uint:\n"
                                "    let mut ws: accel.ptlas_write[2]\n"
                                "    return 0u\n",
                                prefix))
              .contains("an array holding a ptlas_write"));
    CHECK(reports_of(cc::format("{}binding b:\n"
                                "    w: accel.ptlas_write\n",
                                prefix))
              .contains("only a buffer's element follows"));
}

TEST("sgl target layout - only a module SGL ships declares one")
{
    CHECK(reports_of("@target_layout\n"
                     "struct pair:\n"
                     "    const if target_api == .vulkan:\n"
                     "        a: float\n"
                     "        b: float\n"
                     "    else:\n"
                     "        b: float\n"
                     "        a: float\n")
              .contains("@target_layout on a struct outside the modules SGL ships"));
}

namespace
{
constexpr cc::string_view k_clusters = "use accel\n"
                                       "\n"
                                       "binding records:\n"
                                       "    buffers: buffer[accel.buffer_ref]\n"
                                       "    clusters: mut buffer[accel.clas_triangles]\n"
                                       "\n"
                                       "@compute(64) fun main_cs(@thread_id id: int3){records}:\n"
                                       "    let c = accel.clas_triangles(cluster_id = 7u, triangle_count = 2u, "
                                       "vertex_count = 4u, index_buffer = records.buffers[0], vertex_buffer = "
                                       "records.buffers[1])\n"
                                       "    records.clusters[id.x] = c\n";
} // namespace

TEST("sgl target layout - @bits packs fields into words, and @absent drops one a target lacks (CHK-526)")
{
    auto const vulkan = compiled(k_clusters, target::hlsl_vulkan);
    auto const dx12 = compiled(k_clusters, target::hlsl_dx12);
    REQUIRE(vulkan.has_value());
    REQUIRE(dx12.has_value());
    // Vulkan's 64-byte record: four packed words after the id and the flags, then five addresses
    CHECK(vulkan.value().text.contains("[[vk::offset(8)]] uint sgl_bits2;"));
    CHECK(vulkan.value().text.contains("[[vk::offset(20)]] uint sgl_bits5;"));
    CHECK(vulkan.value().text.contains("[[vk::offset(56)]] buffer_ref micromap_index_buffer;"));
    CHECK(!vulkan.value().text.contains("micromap_base"));
    // 2 triangles in bits 0-8, 4 vertices in 9-17, the default 4-byte index format in 24-27, all of it folded
    CHECK(vulkan.value().text.contains("c.sgl_bits2 = 67110914u;"));
    // the index stride 4 below the vertex stride 12
    CHECK(vulkan.value().text.contains("c.sgl_bits4 = 786436u;"));
    // D3D12's 80-byte record: its micromap base stands where Vulkan has none, and no index format at all
    CHECK(dx12.value().text.contains("uint micromap_base;"));
    CHECK(!dx12.value().text.contains("index_format"));
    CHECK(dx12.value().text.contains("buffer_ref geometry_index_buffer;"));
}

TEST("sgl target layout - a packed or an absent field is never read or written alone")
{
    auto source = cc::string(k_clusters);
    source.replace_first("    records.clusters[id.x] = c\n", "    let mut d = c\n    d.triangle_count = 3u\n    "
                                                             "records.clusters[id.x] = d\n");
    auto const alone = compiled(source, target::hlsl_vulkan);
    REQUIRE(alone.has_error());
    CHECK(alone.error().contains("a read or a write of clas_triangles.triangle_count alone"));
    CHECK(reports_of("struct pair:\n"
                     "    @bits(4) a: uint\n"
                     "\n"
                     "fun f(p: pair) -> uint => p.a\n")
              .contains("which a branch of a @target_layout struct alone says"));
}
