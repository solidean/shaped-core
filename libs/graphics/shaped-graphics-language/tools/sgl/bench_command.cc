#include "files.hh"

#include <clean-core/common/time.hh>
#include <clean-core/string/print.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/emit/emit.hh>

using namespace cc::primitive_defines;

namespace
{
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;

struct bench_shader
{
    cc::string path;
    cc::string entry;
    cc::string source;
};
} // namespace

// `sgl bench`: how long each shader of a list takes to compile, in this process, the way a compiler survey times it.
// Every source is read before the clock starts, every compile is one `compile_to_text` on one thread, and a shader's time
// is the fastest of its rounds.
// The first compile checks the prelude, which a process does once; it is reported on its own line and in no shader's time.
COMMAND("bench")
{
    auto list_path = cc::string();
    auto target_names = cc::vector<cc::string>();
    auto rounds = 10;
    auto args
        = nx::args({.name = "sgl bench",
                    .description = "Compiles each shader of a list on each target, several rounds in this process, "
                                   "and prints the fastest time of each as `path<TAB>target<TAB>seconds`."});
    args.positional("LIST", list_path, {.desc = "a file of `path<TAB>entry` lines"});
    args.arg({"target"}, target_names,
             {.desc = "hlsl-dx12, hlsl-vulkan, wgsl or msl; repeat for each, all four when none is given",
              .metavar = "TARGET"});
    args.arg({"rounds"}, rounds, {.desc = "compiles per shader and target, of which the fastest counts", .metavar = "N"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();
    if (rounds < 1)
    {
        cc::eprintln("sgl bench: --rounds must be at least 1");
        return exit_usage;
    }

    auto targets = cc::vector<sgl::emit::target>();
    for (auto const& name : target_names)
    {
        auto is_known = false;
        for (auto const t : sgl::emit::all_targets())
            if (sgl::emit::to_string(t) == name)
            {
                targets.push_back(t);
                is_known = true;
            }
        if (!is_known)
        {
            cc::eprintln("sgl bench: unknown target '{}' (one of hlsl-dx12, hlsl-vulkan, wgsl, msl)", name);
            return exit_usage;
        }
    }
    if (targets.empty())
        for (auto const t : sgl::emit::all_targets())
            targets.push_back(t);

    auto const list = sgl_tool::read_file(list_path);
    if (list.has_error())
    {
        cc::eprintln("sgl bench: cannot read {}: {}", list_path, list.error());
        return exit_usage;
    }
    auto shaders = cc::vector<bench_shader>();
    auto const text = cc::string_view(list.value());
    auto line_start = isize(0);
    for (auto i = isize(0); i <= text.size(); ++i)
    {
        if (i < text.size() && text[i] != '\n')
            continue;
        auto line = text.subview({.offset = line_start, .size = i - line_start});
        line_start = i + 1;
        if (!line.empty() && line[line.size() - 1] == '\r')
            line = line.subview({.offset = 0, .size = line.size() - 1});
        if (line.empty())
            continue;
        auto const tab = line.find('\t');
        if (tab <= 0)
        {
            cc::eprintln("sgl bench: a line of the list is `path<TAB>entry`, and '{}' is not", line);
            return exit_usage;
        }
        auto const path = line.subview({.offset = 0, .size = tab});
        auto source = sgl_tool::read_file(path);
        if (source.has_error())
        {
            cc::eprintln("sgl bench: cannot read {}: {}", path, source.error());
            return exit_usage;
        }
        shaders.push_back(
            {.path = cc::string(path), .entry = cc::string(line.subview(tab + 1)), .source = source.value()});
    }

    auto const compile = [](bench_shader const& s, sgl::emit::target t)
    { return sgl::compile_to_text({.source = s.source, .source_name = s.path, .entry_point = s.entry, .target = t}); };

    if (!shaders.empty())
    {
        auto const start = cc::current_time_steady_secs();
        (void)compile(shaders[0], targets[0]);
        cc::println("prelude\t-\t{:.9f}", cc::current_time_steady_secs() - start);
    }

    for (auto const& s : shaders)
        for (auto const t : targets)
        {
            auto best = 1e30;
            auto failed = false;
            for (auto r = 0; r < rounds && !failed; ++r)
            {
                auto const start = cc::current_time_steady_secs();
                auto const result = compile(s, t);
                auto const secs = cc::current_time_steady_secs() - start;
                failed = result.has_error();
                best = cc::min(best, secs);
            }
            if (failed)
                cc::println("{}\t{}\tfailed", s.path, sgl::emit::to_string(t));
            else
                cc::println("{}\t{}\t{:.9f}", s.path, sgl::emit::to_string(t), best);
        }
    cc::flush();
    return exit_ok;
}
