#include <clean-core/algorithm/sort.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

#include <filesystem>

using namespace cc::primitive_defines;

// EMIT-257: the line table the native routes write when asked, and what they write when not.
// DXC's validator and disassembly read the DXIL half in slib's tests, which link DXC.

namespace
{
// line 7 is a statement of an inlined function, and line 12 the call that inlined it
constexpr cc::string_view k_lines = "binding work:\n"                                   // 1
                                    "    values: mut buffer[float]\n"                   // 2
                                    "\n"                                                // 3
                                    "fun clamped(x: float) -> float:\n"                 // 4
                                    "    let mut y = x\n"                               // 5
                                    "    if y > 1.0:\n"                                 // 6
                                    "        y = y * 0.5\n"                             // 7
                                    "    return y\n"                                    // 8
                                    "\n"                                                // 9
                                    "@compute(1) fun run(@thread_id id: int3){work}:\n" // 10
                                    "    let x = work.values[0]\n"                      // 11
                                    "    work.values[1] = clamped(x)\n";                // 12

constexpr u32 k_op_source_continued = 2;
constexpr u32 k_op_source = 3;
constexpr u32 k_op_string = 7;
constexpr u32 k_op_line = 8;
constexpr u32 k_op_no_line = 317;
constexpr u32 k_op_fmul = 133;

[[nodiscard]] sgl::emitted_source compiled(cc::string_view source,
                                           cc::string_view entry,
                                           sgl::emit::target target,
                                           bool line_table)
{
    auto e = sgl::compile_to_text({.source = source,
                                   .source_name = "lines.sgl",
                                   .entry_point = entry,
                                   .target = target,
                                   .native = sgl::emit::native_route::require,
                                   .native_line_table = line_table});
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    return cc::move(e.value());
}

[[nodiscard]] bool is_debug_line(u32 opcode)
{
    return opcode == k_op_source_continued || opcode == k_op_source || opcode == k_op_string || opcode == k_op_line
        || opcode == k_op_no_line;
}

/// The module's instructions after its header, with the debug ones left out.
[[nodiscard]] cc::vector<u32> without_lines(cc::span<u32 const> words)
{
    auto kept = cc::vector<u32>();
    for (auto at = isize(5); at < words.size();)
    {
        auto const count = isize(words[at] >> 16);
        if (count == 0)
            break;
        if (!is_debug_line(words[at] & 0xffffu))
            for (auto k = isize(0); k < count && at + k < words.size(); ++k)
                kept.push_back(words[at + k]);
        at += count;
    }
    return kept;
}

/// A DXBC container's part `name`, or nothing.
[[nodiscard]] cc::vector<byte> part_of(cc::span<byte const> container, cc::string_view name)
{
    auto const u32_at = [&](isize at)
    {
        return u32(container[at]) | u32(container[at + 1]) << 8 | u32(container[at + 2]) << 16
             | u32(container[at + 3]) << 24;
    };
    if (container.size() < 32)
        return {};
    auto const count = u32_at(28);
    for (auto k = u32(0); k < count; ++k)
    {
        auto const at = isize(u32_at(32 + 4 * isize(k)));
        auto const is_it = container[at] == byte(name[0]) && container[at + 1] == byte(name[1])
                        && container[at + 2] == byte(name[2]) && container[at + 3] == byte(name[3]);
        if (is_it)
            return cc::vector<byte>::create_copy_of(container.subspan({.offset = at + 8, .size = isize(u32_at(at + 4))}));
    }
    return {};
}

template <class T>
[[nodiscard]] bool same(cc::span<T const> a, cc::span<T const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

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
} // namespace

TEST("sgl native lines - SPIR-V names each operation's .sgl line, an inlined one's included (EMIT-257)")
{
    auto const e = compiled(k_lines, "run", sgl::emit::target::hlsl_vulkan, true);
    auto const& words = e.spirv;
    REQUIRE(!words.empty());

    auto file = u32(0);
    auto lines = cc::vector<u32>();
    auto line = u32(0);
    auto multiplied_on = u32(0);
    auto has_source = false;
    for (auto at = isize(5); at < words.size();)
    {
        auto const count = isize(words[at] >> 16);
        auto const opcode = words[at] & 0xffffu;
        REQUIRE(count > 0);
        if (opcode == k_op_string)
            file = words[at + 1];
        if (opcode == k_op_source)
            has_source = has_source || words[at + 3] == file;
        if (opcode == k_op_line)
        {
            CHECK(words[at + 1] == file);
            line = words[at + 2];
            lines.push_back(line);
        }
        if (opcode == k_op_fmul)
            multiplied_on = line;
        at += count;
    }
    CHECK(file != 0);
    // the text itself stands beside the name, so a debugger needs no file
    CHECK(has_source);
    auto const names = [&](u32 l)
    {
        for (auto const n : lines)
            if (n == l)
                return true;
        return false;
    };
    CHECK(names(11));
    CHECK(names(12));
    // SPIR-V's core lines carry no call, so an inlined statement names its own line
    CHECK(multiplied_on == 7u);
}

TEST("sgl native lines - without the option a module carries no line, and with it no instruction else changes")
{
    for (auto const target : {sgl::emit::target::hlsl_vulkan, sgl::emit::target::hlsl_dx12})
    {
        auto const off = compiled(k_lines, "run", target, false);
        auto const on = compiled(k_lines, "run", target, true);
        if (target == sgl::emit::target::hlsl_vulkan)
        {
            CHECK(same<u32>(without_lines(off.spirv), cc::span<u32 const>(off.spirv).subspan(5)));
            CHECK(same<u32>(without_lines(on.spirv), without_lines(off.spirv)));
            CHECK(on.spirv.size() > off.spirv.size());
            continue;
        }
        // DXC's -Zi: the program part keeps its bytes, and the debug module stands beside it
        CHECK(same<byte>(part_of(on.dxil, "DXIL"), part_of(off.dxil, "DXIL")));
        CHECK(part_of(off.dxil, "ILDB").empty());
        CHECK(part_of(off.dxil, "ILDN").empty());
        CHECK(!part_of(on.dxil, "ILDB").empty());
        CHECK(!part_of(on.dxil, "ILDN").empty());
    }
}

TEST("sgl native lines - every entry point of the repo's SGL changes no instruction for its line table",
     nx::config::rerun_on("libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/**"))
{
    auto files = sources_under(nx::source_path(SGL_SG_SHADERS_DIR));
    files.push_back_range(sources_under(nx::source_path(SGL_SR_SHADERS_DIR)));
    files.push_back_range(sources_under(nx::source_path(SGL_CORPUS_DIR)));
    REQUIRE(!files.empty());

    auto const stride = nx::is_thorough() ? 1 : 8;
    auto compared = 0;
    for (auto i = isize(0); i < files.size(); i += stride)
    {
        auto const source = read_all(files[i]);
        auto const all = sgl::compile_all_to_text({.source = source, .targets = {sgl::emit::target::hlsl_vulkan}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
            for (auto const target : {sgl::emit::target::hlsl_vulkan, sgl::emit::target::hlsl_dx12})
            {
                auto const request = [&](bool line_table)
                {
                    return sgl::compile_to_text({.source = source,
                                                 .source_name = files[i],
                                                 .entry_point = entry.entry_point,
                                                 .target = target,
                                                 .native = sgl::emit::native_route::prefer,
                                                 .native_line_table = line_table});
                };
                auto const off = request(false);
                auto const on = request(true);
                if (off.has_error() || on.has_error() || (off.value().spirv.empty() && off.value().dxil.empty()))
                    continue;
                ++compared;
                if (target == sgl::emit::target::hlsl_vulkan)
                    CHECK(same<u32>(without_lines(on.value().spirv), without_lines(off.value().spirv)))
                        .context(cc::format("{} {}", files[i], entry.entry_point));
                else
                    CHECK(same<byte>(part_of(on.value().dxil, "DXIL"), part_of(off.value().dxil, "DXIL")))
                        .context(cc::format("{} {}", files[i], entry.entry_point));
            }
    }
    CC_LOG_INFO("{} native compiles keep every instruction with their line table", compared);
    CHECK(compared >= (nx::is_thorough() ? 500 : 40));
}
