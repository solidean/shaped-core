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
