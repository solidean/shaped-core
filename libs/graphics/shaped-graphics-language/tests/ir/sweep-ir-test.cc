#include "ir-test-support.hh"

#include <babel-data/shader/dxil_container.hh>
#include <babel-data/shader/llvm37.hh>
#include <babel-data/shader/llvm_bitstream.hh>
#include <babel-data/shader/spirv.hh>
#include <clean-core/algorithm/sort.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <nexus/source_root.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

#include <filesystem>

using namespace cc::primitive_defines;

// Every entry point the repo's SGL holds, down both routes, where its stage is lowered yet, its SPIR-V and DXIL read back by babel.
// Where assertions are on a compile verifies its program after lowering and after each pass, so this is what walks the
// verifier over everything; `spirv-val` and DXC's validator only see the shaders sg and sr ship.
// A debug build pays seconds per file for that, so a default run takes every eighth and --thorough takes them all.

namespace
{
[[nodiscard]] cc::vector<cc::string> sources_under(cc::string_view root)
{
    namespace fs = std::filesystem;
    auto found = cc::vector<cc::string>();
    for (auto const& e : fs::recursive_directory_iterator(fs::path(root.data(), root.data() + root.size())))
        if (e.is_regular_file() && e.path().extension() == ".sgl")
            found.push_back(cc::string(e.path().generic_string().c_str()));
    cc::sort(found);
    return found;
}

[[nodiscard]] cc::string read_all(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}
/// babel's reader takes SGL's SPIR-V as a module: every instruction whole, and every id it gives below the bound.
void reads_back(cc::span<u32 const> words, cc::string_view entry)
{
    auto const m = babel::spirv::read(words);
    REQUIRE(m.has_value()).dump("entry", entry).dump("error", m.has_error() ? m.error().to_string() : cc::string());
    auto unknown = 0;
    auto past_bound = 0;
    for (auto k = isize(0); k < m.value().instruction_count(); ++k)
    {
        auto const i = m.value().at(k);
        unknown += babel::spirv::info_of(i.opcode()) == nullptr ? 1 : 0;
        past_bound += i.result_id() >= m.value().head.bound ? 1 : 0;
    }
    CHECK(unknown == 0).dump("entry", entry);
    CHECK(past_bound == 0).dump("entry", entry);
}
/// babel's readers take SGL's DXIL as a container, its bitcode as LLVM 3.7, and every operand as a value it holds.
void reads_back_dxil(cc::span<byte const> bytes, cc::string_view entry)
{
    auto const c = babel::dxil::read(bytes);
    REQUIRE(c.has_value()).dump("entry", entry);
    auto const* const part = c.value().part_named("DXIL");
    REQUIRE(part != nullptr).dump("entry", entry);
    auto const p = babel::dxil::program_of(*part);
    REQUIRE(p.has_value()).dump("entry", entry);
    auto const bc = babel::llvm::read_bitstream(p.value().bitcode);
    REQUIRE(bc.has_value()).dump("entry", entry).dump("error", bc.has_error() ? bc.error().to_string() : cc::string());
    auto const m = babel::llvm37::read(bc.value());
    REQUIRE(m.has_value()).dump("entry", entry).dump("error", m.has_error() ? m.error().to_string() : cc::string());
    auto unresolved = 0;
    for (auto const& b : m.value().bodies)
        for (auto const& i : b.instructions)
        {
            auto const operands = m.value().operands_of(b, i);
            for (auto k = isize(0); k < operands.size(); ++k)
                if (b.roles[i.first + k] == babel::llvm37::operand_role::value
                    && m.value().value_of(b, operands[k]).index == 0xffffffffu)
                    ++unresolved;
        }
    CHECK(unresolved == 0).dump("entry", entry);
}
} // namespace

TEST("sgl ir - every entry point of the repo's SGL goes down both native routes",
     nx::config::rerun_on("libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/**"))
{
    auto files = sources_under(nx::source_path(SGL_SG_SHADERS_DIR));
    files.push_back_range(sources_under(nx::source_path(SGL_SR_SHADERS_DIR)));
    files.push_back_range(sources_under(nx::source_path(SGL_CORPUS_DIR)));
    REQUIRE(!files.empty());

    auto const stride = nx::is_thorough() ? 1 : 8;
    auto spirv = 0;
    auto dxil = 0;
    for (auto i = isize(0); i < files.size(); i += stride)
    {
        auto const& file = files[i];
        auto const source = read_all(file);
        // what names a module of another directory is the module tests', and this one reads a file alone
        auto const all = sgl::compile_all_to_text({.source = source, .targets = {sgl::emit::target::hlsl_vulkan}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
            for (auto const target : {sgl::emit::target::hlsl_vulkan, sgl::emit::target::hlsl_dx12})
            {
                auto const e = sgl::compile_to_text(
                    {.source = source,
                     .entry_point = entry.entry_point,
                     .target = target,
                     .diagnostics = true,
                     .device_features = sgl::check::feature_set(sgl::check::feature::vertex_pipeline_stores),
                     .native = sgl::emit::native_route::prefer});
                if (e.has_error())
                    continue;
                if (!e.value().spirv.empty())
                    reads_back(e.value().spirv, entry.entry_point);
                if (!e.value().dxil.empty())
                    reads_back_dxil(e.value().dxil, entry.entry_point);
                spirv += e.value().spirv.empty() ? 0 : 1;
                dxil += e.value().dxil.empty() ? 0 : 1;
            }
    }
    // the numbers the sweep reaches, so a lowering lost shows here rather than as a quiet fall back to DXC
    CC_LOG_INFO("{} entry points lowered to SPIR-V and {} to DXIL", spirv, dxil);
    // a sample's count hangs on which files every eighth lands on, and one new file moves them all, so its floors are looser than a share of the whole
    CHECK(spirv >= (nx::is_thorough() ? 260 : 24));
    CHECK(dxil >= (nx::is_thorough() ? 230 : 20));
}
