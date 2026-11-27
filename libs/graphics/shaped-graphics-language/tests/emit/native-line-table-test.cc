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
constexpr u32 k_op_ext_inst_import = 11;
constexpr u32 k_op_ext_inst = 12;
constexpr u32 k_op_type_int = 21;
constexpr u32 k_op_constant = 43;
constexpr u32 k_op_no_line = 317;
constexpr u32 k_op_fmul = 133;

[[nodiscard]] sgl::emitted_source compiled(cc::string_view source,
                                           cc::string_view entry,
                                           sgl::emit::target target,
                                           bool line_table)
{
    auto e = sgl::compile_to_text(
        {.source = source,
         .source_name = "lines.sgl",
         .entry_point = entry,
         .target = target,
         .native = sgl::emit::native_route::require,
         .native_debug_info = line_table ? sgl::emit::debug_info::lines : sgl::emit::debug_info::none});
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    return cc::move(e.value());
}

[[nodiscard]] bool is_debug_line(u32 opcode)
{
    return opcode == k_op_source_continued || opcode == k_op_source || opcode == k_op_string || opcode == k_op_line
        || opcode == k_op_no_line;
}

/// The id of the module's `NonSemantic.Shader.DebugInfo.100` import, or 0.
[[nodiscard]] u32 debug_set_of(cc::span<u32 const> words)
{
    for (auto at = isize(5); at < words.size();)
    {
        auto const count = isize(words[at] >> 16);
        if (count == 0)
            break;
        if ((words[at] & 0xffffu) == k_op_ext_inst_import
            && cc::string_view(reinterpret_cast<char const*>(&words[at + 2])).starts_with("NonSemantic.Shader.DebugInfo"))
            return words[at + 1];
        at += count;
    }
    return 0;
}

/// The module's instructions after its header, with the debug ones left out.
/// The debug info's ids come after every other (EMIT-267), so what takes an id from its import on is the debug info's.
[[nodiscard]] cc::vector<u32> without_lines(cc::span<u32 const> words)
{
    auto const set = debug_set_of(words);
    auto const is_debug_info = [&](isize at)
    {
        auto const opcode = words[at] & 0xffffu;
        if (set == 0)
            return false;
        if (opcode == k_op_ext_inst_import || opcode == k_op_type_int)
            return words[at + 1] >= set;
        return (opcode == k_op_ext_inst || opcode == k_op_constant) && words[at + 2] >= set;
    };
    auto kept = cc::vector<u32>();
    for (auto at = isize(5); at < words.size();)
    {
        auto const count = isize(words[at] >> 16);
        if (count == 0)
            break;
        if (!is_debug_line(words[at] & 0xffffu) && !is_debug_info(at))
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

TEST("sgl native lines - SPIR-V at full describes each variable, its scope and the call it was inlined through "
     "(EMIT-267)")
{
    constexpr u32 k_debug_function = 20;
    constexpr u32 k_debug_inlined_at = 25;
    constexpr u32 k_debug_local_variable = 26;
    constexpr u32 k_debug_value = 29;
    constexpr u32 k_debug_line = 103;

    // a vector read whole, which a debugger shows lane by lane
    auto source = cc::string(k_lines);
    source += "    let v = float3(work.values[2], work.values[3], 2.0)\n"
              "    work.values[4] = dot(v, v)\n";
    auto e = sgl::compile_to_text({.source = source,
                                   .source_name = "lines.sgl",
                                   .entry_point = "run",
                                   .target = sgl::emit::target::hlsl_vulkan,
                                   .native = sgl::emit::native_route::require,
                                   .native_debug_info = sgl::emit::debug_info::full});
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    auto const& words = e.value().spirv;
    auto const set = debug_set_of(words);
    REQUIRE(set != 0);

    struct named
    {
        u32 id;
        cc::string_view text;
    };
    struct constant
    {
        u32 id;
        u32 value;
    };
    auto strings = cc::vector<named>();
    auto constants = cc::vector<constant>();
    auto const text_of = [&](u32 id)
    {
        for (auto const& n : strings)
            if (n.id == id)
                return n.text;
        return cc::string_view();
    };
    auto const value_of = [&](u32 id)
    {
        for (auto const& c : constants)
            if (c.id == id)
                return c.value;
        return u32(0);
    };
    auto variables = cc::vector<cc::string_view>();
    auto functions = cc::vector<cc::string_view>();
    auto lines = cc::vector<u32>();
    auto inlined = 0;
    auto values = 0;
    auto indexed_values = 0;
    auto core_lines = 0;
    for (auto at = isize(5); at < words.size();)
    {
        auto const count = isize(words[at] >> 16);
        auto const opcode = words[at] & 0xffffu;
        REQUIRE(count > 0);
        if (opcode == k_op_string)
            strings.push_back({words[at + 1], cc::string_view(reinterpret_cast<char const*>(&words[at + 2]))});
        if (opcode == k_op_constant && count == 4)
            constants.push_back({words[at + 2], words[at + 3]});
        core_lines += opcode == k_op_line ? 1 : 0;
        if (opcode == k_op_ext_inst && words[at + 3] == set)
        {
            auto const instruction = words[at + 4];
            if (instruction == k_debug_local_variable)
                variables.push_back(text_of(words[at + 5]));
            if (instruction == k_debug_function)
                functions.push_back(text_of(words[at + 5]));
            if (instruction == k_debug_inlined_at)
                ++inlined;
            if (instruction == k_debug_value)
            {
                ++values;
                indexed_values += count > 8 ? 1 : 0;
            }
            if (instruction == k_debug_line)
                lines.push_back(value_of(words[at + 6]));
        }
        at += count;
    }
    auto const holds = [](auto const& in, auto const& wanted)
    {
        for (auto const& n : in)
            if (n == wanted)
                return true;
        return false;
    };
    // the debug info's own lines stand in place of the core ones, as DXC writes them
    CHECK(core_lines == 0);
    CHECK(holds(lines, 11u));
    CHECK(holds(lines, 12u));
    CHECK(holds(lines, 7u));
    CHECK(holds(functions, cc::string_view("run")));
    CHECK(holds(functions, cc::string_view("clamped")));
    CHECK(inlined > 0);
    CHECK(holds(variables, cc::string_view("x")));
    CHECK(holds(variables, cc::string_view("y")));
    CHECK(holds(variables, cc::string_view("v")));
    CHECK(values > 0);
    // `v` is read whole, so its value is stated whole and needs no index
    CHECK(indexed_values == 0);
}

TEST("sgl native lines - a let read lane by lane, which the legalizer splits, is one variable stated lane by lane "
     "(EMIT-267)")
{
    constexpr u32 k_debug_local_variable = 26;
    constexpr u32 k_debug_value = 29;

    auto const source = cc::string("binding work:\n"
                                   "    values: mut buffer[float]\n"
                                   "\n"
                                   "@compute(1) fun run(@thread_id id: int3){work}:\n"
                                   "    let v = float3(work.values[0], work.values[1], work.values[2])\n"
                                   "    work.values[3] = v.x * v.y + v.z\n");
    auto e = sgl::compile_to_text({.source = source,
                                   .source_name = "lanes.sgl",
                                   .entry_point = "run",
                                   .target = sgl::emit::target::hlsl_vulkan,
                                   .native = sgl::emit::native_route::require,
                                   .native_debug_info = sgl::emit::debug_info::full});
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    auto const& words = e.value().spirv;
    auto const set = debug_set_of(words);
    REQUIRE(set != 0);

    auto v_string = u32(0);
    auto v_variable = u32(0);
    auto constants = cc::vector<cc::pair<u32, u32>>();
    auto lanes = cc::vector<u32>();
    auto variables = 0;
    for (auto at = isize(5); at < words.size();)
    {
        auto const count = isize(words[at] >> 16);
        auto const opcode = words[at] & 0xffffu;
        REQUIRE(count > 0);
        if (opcode == k_op_string && cc::string_view(reinterpret_cast<char const*>(&words[at + 2])) == "v")
            v_string = words[at + 1];
        if (opcode == k_op_constant && count == 4)
            constants.push_back({words[at + 2], words[at + 3]});
        if (opcode == k_op_ext_inst && words[at + 3] == set && words[at + 4] == k_debug_local_variable)
        {
            ++variables;
            if (words[at + 5] == v_string)
                v_variable = words[at + 2];
        }
        if (opcode == k_op_ext_inst && words[at + 3] == set && words[at + 4] == k_debug_value
            && words[at + 5] == v_variable && count == 9)
            for (auto const& [id, value] : constants)
                if (id == words[at + 8])
                    lanes.push_back(value);
        at += count;
    }
    // the lanes' own lets are no variables of their own, and `v`'s value is stated from each lane on
    CHECK(v_variable != 0u);
    CHECK(variables == 1).dump("variables", variables);
    cc::sort(lanes);
    REQUIRE(lanes.size() == 3);
    CHECK(lanes[0] == 0u);
    CHECK(lanes[1] == 1u);
    CHECK(lanes[2] == 2u);
}

TEST("sgl native lines - an inlined call's parameter is a variable of its callee, not of the caller where it is bound "
     "(EMIT-267)")
{
    constexpr u32 k_debug_function = 20;
    constexpr u32 k_debug_local_variable = 26;

    // `v` is read twice, so the call binds it to a local of its own rather than reading the argument where it stands;
    // `w` gives the callee a statement of its own, which a body of one expression folded into the call would not
    auto const source = cc::string("binding work:\n"
                                   "    values: mut buffer[float]\n"
                                   "\n"
                                   "fun twice(v: float) -> float:\n"
                                   "    let w = v * v\n"
                                   "    return w + v\n"
                                   "\n"
                                   "@compute(1) fun run(@thread_id id: int3){work}:\n"
                                   "    work.values[1] = twice(work.values[0] * 2.0)\n");
    auto e = sgl::compile_to_text({.source = source,
                                   .source_name = "inlined.sgl",
                                   .entry_point = "run",
                                   .target = sgl::emit::target::hlsl_vulkan,
                                   .native = sgl::emit::native_route::require,
                                   .native_debug_info = sgl::emit::debug_info::full});
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    auto const& words = e.value().spirv;
    auto const set = debug_set_of(words);
    REQUIRE(set != 0);

    auto strings = cc::vector<cc::pair<u32, cc::string_view>>();
    auto const named = [&](u32 id, cc::string_view text)
    {
        for (auto const& [known, held] : strings)
            if (known == id)
                return held == text;
        return false;
    };
    auto twice_function = u32(0);
    auto v_scope = u32(0);
    for (auto at = isize(5); at < words.size();)
    {
        auto const count = isize(words[at] >> 16);
        auto const opcode = words[at] & 0xffffu;
        REQUIRE(count > 0);
        if (opcode == k_op_string)
            strings.push_back({words[at + 1], cc::string_view(reinterpret_cast<char const*>(&words[at + 2]))});
        if (opcode == k_op_ext_inst && words[at + 3] == set && words[at + 4] == k_debug_function
            && named(words[at + 5], "twice"))
            twice_function = words[at + 2];
        // DebugLocalVariable: name, type, source, line, column, scope
        if (opcode == k_op_ext_inst && words[at + 3] == set && words[at + 4] == k_debug_local_variable
            && named(words[at + 5], "v"))
            v_scope = words[at + 10];
        at += count;
    }
    REQUIRE(twice_function != 0u);
    CHECK(v_scope == twice_function);
}

TEST("sgl native lines - a call inside a generic function's instance names the function it calls (EMIT-267)")
{
    constexpr u32 k_debug_function = 20;

    auto const source = cc::string("binding work:\n"
                                   "    values: mut buffer[float]\n"
                                   "\n"
                                   "fun halved(v: float) -> float:\n"
                                   "    let h = v * 0.5\n"
                                   "    return h + v * h\n"
                                   "\n"
                                   "fun twice[T](x: T) -> T:\n"
                                   "    let y = halved(x)\n"
                                   "    return y + y * x\n"
                                   "\n"
                                   "@compute(1) fun run(@thread_id id: int3){work}:\n"
                                   "    let a = work.values[0]\n"
                                   "    work.values[1] = twice[float](a)\n");
    auto e = sgl::compile_to_text({.source = source,
                                   .source_name = "generic.sgl",
                                   .entry_point = "run",
                                   .target = sgl::emit::target::hlsl_vulkan,
                                   .native = sgl::emit::native_route::require,
                                   .native_debug_info = sgl::emit::debug_info::full});
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    auto const& words = e.value().spirv;
    auto const set = debug_set_of(words);
    REQUIRE(set != 0);

    auto strings = cc::vector<cc::pair<u32, cc::string_view>>();
    auto functions = cc::vector<cc::string_view>();
    for (auto at = isize(5); at < words.size();)
    {
        auto const count = isize(words[at] >> 16);
        auto const opcode = words[at] & 0xffffu;
        REQUIRE(count > 0);
        if (opcode == k_op_string)
            strings.push_back({words[at + 1], cc::string_view(reinterpret_cast<char const*>(&words[at + 2]))});
        if (opcode == k_op_ext_inst && words[at + 3] == set && words[at + 4] == k_debug_function)
            for (auto const& [id, text] : strings)
                if (id == words[at + 5])
                    functions.push_back(text);
        at += count;
    }
    auto const holds = [&](cc::string_view name)
    {
        for (auto const f : functions)
            if (f == name)
                return true;
        return false;
    };
    CHECK(holds("twice"));
    CHECK(holds("halved"));
    CHECK(!holds("<inlined>"));
}

TEST("sgl native lines - a variable declared in an arm is scoped to the arm's block, in its function (EMIT-267)")
{
    constexpr u32 k_debug_function = 20;
    constexpr u32 k_debug_lexical_block = 21;
    constexpr u32 k_debug_local_variable = 26;

    auto const source = cc::string("binding work:\n"
                                   "    values: mut buffer[float]\n"
                                   "\n"
                                   "@compute(1) fun run(@thread_id id: int3){work}:\n"
                                   "    let a = work.values[0]\n"
                                   "    if a > 1.0:\n"
                                   "        let b = a * work.values[2]\n"
                                   "        work.values[1] = b * b\n");
    auto e = sgl::compile_to_text({.source = source,
                                   .source_name = "blocks.sgl",
                                   .entry_point = "run",
                                   .target = sgl::emit::target::hlsl_vulkan,
                                   .native = sgl::emit::native_route::require,
                                   .native_debug_info = sgl::emit::debug_info::full});
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    auto const& words = e.value().spirv;
    auto const set = debug_set_of(words);
    REQUIRE(set != 0);

    auto strings = cc::vector<cc::pair<u32, cc::string_view>>();
    auto const named = [&](u32 id, cc::string_view text)
    {
        for (auto const& [known, held] : strings)
            if (known == id)
                return held == text;
        return false;
    };
    auto function = u32(0);
    auto blocks = cc::vector<cc::pair<u32, u32>>();
    auto a_scope = u32(0);
    auto b_scope = u32(0);
    for (auto at = isize(5); at < words.size();)
    {
        auto const count = isize(words[at] >> 16);
        auto const opcode = words[at] & 0xffffu;
        REQUIRE(count > 0);
        if (opcode == k_op_string)
            strings.push_back({words[at + 1], cc::string_view(reinterpret_cast<char const*>(&words[at + 2]))});
        if (opcode == k_op_ext_inst && words[at + 3] == set)
        {
            auto const instruction = words[at + 4];
            if (instruction == k_debug_function)
                function = words[at + 2];
            // DebugLexicalBlock: source, line, column, parent
            if (instruction == k_debug_lexical_block)
                blocks.push_back({words[at + 2], words[at + 8]});
            if (instruction == k_debug_local_variable && named(words[at + 5], "a"))
                a_scope = words[at + 10];
            if (instruction == k_debug_local_variable && named(words[at + 5], "b"))
                b_scope = words[at + 10];
        }
        at += count;
    }
    REQUIRE(function != 0u);
    CHECK(a_scope == function);
    auto b_parent = u32(0);
    for (auto const& [block, parent] : blocks)
        if (block == b_scope)
            b_parent = parent;
    CHECK(b_parent == function).dump("blocks", blocks.size());
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
                    return sgl::compile_to_text(
                        {.source = source,
                         .source_name = files[i],
                         .entry_point = entry.entry_point,
                         .target = target,
                         .native = sgl::emit::native_route::prefer,
                         .native_debug_info = line_table ? sgl::emit::debug_info::lines : sgl::emit::debug_info::none});
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

TEST("sgl native lines - describing every variable changes no instruction against the lines alone (EMIT-267)",
     nx::config::rerun_on("libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/**"))
{
    auto files = sources_under(nx::source_path(SGL_SG_SHADERS_DIR));
    files.push_back_range(sources_under(nx::source_path(SGL_SR_SHADERS_DIR)));
    files.push_back_range(sources_under(nx::source_path(SGL_CORPUS_DIR)));
    REQUIRE(!files.empty());

    // every pass sees the debug operations, and `ir::verify` checks the program after each of them where asserts are on
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
                auto const request = [&](sgl::emit::debug_info level)
                {
                    return sgl::compile_to_text({.source = source,
                                                 .source_name = files[i],
                                                 .entry_point = entry.entry_point,
                                                 .target = target,
                                                 .native = sgl::emit::native_route::prefer,
                                                 .native_debug_info = level});
                };
                auto const lines = request(sgl::emit::debug_info::lines);
                auto const full = request(sgl::emit::debug_info::full);
                auto const at = cc::format("{} {}", files[i], entry.entry_point);
                CHECK(lines.has_value() == full.has_value()).context(at);
                if (lines.has_error() || full.has_error())
                    continue;
                CHECK(lines.value().native_gap == full.value().native_gap).context(at);
                if (lines.value().spirv.empty() && lines.value().dxil.empty())
                    continue;
                ++compared;
                if (target == sgl::emit::target::hlsl_vulkan)
                    CHECK(same<u32>(without_lines(full.value().spirv), without_lines(lines.value().spirv))).context(at);
                else
                    CHECK(same<byte>(part_of(full.value().dxil, "DXIL"), part_of(lines.value().dxil, "DXIL"))).context(at);
            }
    }
    CC_LOG_INFO("{} native compiles keep every instruction with their variables described", compared);
    CHECK(compared > 0);
}

TEST("sgl native lines - a variable EMIT-292 states again at a copy after a trace changes no other instruction "
     "(EMIT-267)")
{
    // a value each variable names, read after a trace: kept, assigned again before the trace, and traced in one arm
    constexpr cc::string_view source = R"(require raytracing_pipeline

binding gP:
    scale: float
    world: acceleration_structure[.triangles]
    data: mut buffer[float4]

struct payload:
    color: float3

rays R:
    surface: payload

@raygen fun kept(@launch_id id: int3){gP}:
    let k = gP.scale * 2.0
    let mut p = payload(float3(0.0))
    trace(gP.world, ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0)), R.surface, mut p)
    gP.data[0] = float4(p.color.x * k, 0.0, 0.0, 1.0)

@raygen fun reassigned(@launch_id id: int3){gP}:
    let mut m = gP.scale * 2.0
    let first = m
    m = 5.0
    let mut p = payload(float3(0.0))
    trace(gP.world, ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0)), R.surface, mut p)
    gP.data[0] = float4(p.color.x * first, m, 0.0, 1.0)

@raygen fun joined(@launch_id id: int3){gP}:
    let k = gP.scale * 2.0
    let mut p = payload(float3(0.0))
    if id.x > 3:
        trace(gP.world, ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0)), R.surface, mut p)
    gP.data[id.y] = float4(p.color.x * k, 0.0, 0.0, 1.0)

@miss fun sky(p: mut payload):
    p.color = float3(1.0)

hit_group hits for R:
    surface = ()

@raytracing pipeline path:
    rays = R
    raygen = kept
    miss.surface = sky
    hit_groups = (hits)
)";
    for (auto const entry : {cc::string_view("kept"), cc::string_view("reassigned"), cc::string_view("joined")})
        for (auto const target : {sgl::emit::target::hlsl_vulkan, sgl::emit::target::hlsl_dx12})
        {
            auto const request = [&](sgl::emit::debug_info level)
            {
                return sgl::compile_to_text({.source = source,
                                             .source_name = "described.sgl",
                                             .entry_point = entry,
                                             .target = target,
                                             .native = sgl::emit::native_route::require,
                                             .native_debug_info = level});
            };
            auto const lines = request(sgl::emit::debug_info::lines);
            auto const full = request(sgl::emit::debug_info::full);
            REQUIRE(lines.has_value()).context(entry);
            REQUIRE(full.has_value()).context(entry);
            // the copies state their variables, which the program the writer saw holds
            CHECK(full.value().text.contains("debug_value v1:")).context(entry);
            if (target == sgl::emit::target::hlsl_vulkan)
                CHECK(same<u32>(without_lines(full.value().spirv), without_lines(lines.value().spirv))).context(entry);
            else
                CHECK(same<byte>(part_of(full.value().dxil, "DXIL"), part_of(lines.value().dxil, "DXIL"))).context(entry);
        }
}
