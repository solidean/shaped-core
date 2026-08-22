#include "ambient.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/impl/os_args.hh>

namespace
{
using nx::isize;

/// What nx::run recorded, and the views over it.
/// Two vectors rather than one: the strings own the bytes, and the views are what callers get, so a
/// caller never has to care that the harness copied them.
struct captured_args
{
    cc::vector<cc::string> owned;
    cc::vector<cc::string_view> views; // argv[1..], which is what current_args() hands out
    cc::string program;
    bool captured = false;
};

captured_args& capture_slot()
{
    static auto slot = captured_args();
    return slot;
}

/// The OS answer, materialized once and viewed the same way.
captured_args const& os_slot()
{
    static auto const slot = []
    {
        auto out = captured_args();
        auto const& args = nx::impl::os_process_args();

        if (!args.empty())
            out.program = args.front();

        for (auto i = isize(1); i < args.size(); ++i)
            out.owned.push_back(args[i]);

        for (auto const& s : out.owned)
            out.views.push_back(s);

        out.captured = !args.empty();
        return out;
    }();

    return slot;
}

captured_args const& effective_slot()
{
    auto const& captured = capture_slot();
    return captured.captured ? captured : os_slot();
}

/// A name written any of the ways a caller might: "verbose", "--verbose" or "-v" all mean the same lookup.
cc::string_view strip_dashes(cc::string_view name)
{
    if (name.starts_with("--"))
        return name.subview(2);

    if (name.starts_with('-') && name.size() > 1)
        return name.subview(1);

    return name;
}

/// The token's own name half, and whether it carried an inline value.
struct split_token
{
    cc::string_view name;
    cc::string_view value;
    bool has_value = false;
    bool is_option = false;
};

split_token split(cc::string_view token)
{
    auto out = split_token();

    if (!token.starts_with('-') || token == "-" || token == "--")
        return out;

    out.is_option = true;
    auto body = token.starts_with("--") ? token.subview(2) : token.subview(1);

    if (auto const eq = body.find('='); eq >= 0)
    {
        out.name = body.subview({.start = 0, .end = eq});
        out.value = body.subview(eq + 1);
        out.has_value = true;
    }
    else
        out.name = body;

    return out;
}
} // namespace

void nx::impl::set_process_args(int argc, char const* const* argv)
{
    auto& slot = capture_slot();

    // A nexus meta-test nests a whole run inside a running one, so a second capture is a no-op rather than
    // an assertion — the outer process's arguments are the true ones either way.
    if (slot.captured)
        return;

    if (argc > 0 && argv[0] != nullptr)
        slot.program = cc::string(argv[0]);

    for (auto i = 1; i < argc; ++i)
        if (argv[i] != nullptr)
            slot.owned.push_back(cc::string(argv[i]));

    for (auto const& s : slot.owned)
        slot.views.push_back(s);

    slot.captured = true;
}

cc::span<cc::string_view const> nx::process_args()
{
    return effective_slot().views;
}

cc::span<cc::string_view const> nx::current_args()
{
    // A test that declared a line gets exactly that line, empty included.
    // Only a test that declared nothing falls through, which is what keeps a helper written for a tool
    // working when it is called from inside one.
    if (auto const test = impl::current_test_args(); test.has_value())
        return test.value();

    return process_args();
}

cc::string_view nx::program_path()
{
    return effective_slot().program;
}

cc::string_view nx::program_name()
{
    auto const path = program_path();

    auto start = isize(0);
    for (auto i = isize(0); i < path.size(); ++i)
        if (path[i] == '/' || path[i] == '\\')
            start = i + 1;

    auto name = path.subview(start);
    if (name.size() > 4)
    {
        auto const tail = name.subview(name.size() - 4);
        if (tail == ".exe" || tail == ".EXE")
            name = name.subview({.start = 0, .end = name.size() - 4});
    }

    return name;
}

cc::optional<cc::string_view> nx::get_arg(cc::string_view name)
{
    auto const wanted = strip_dashes(name);
    auto const args = current_args();

    for (auto i = isize(0); i < args.size(); ++i)
    {
        // Past a bare `--` the tokens are being handed to another program, and reading them here would let
        // an argument meant for it switch on a debug path in us.
        if (args[i] == "--")
            break;

        auto const token = split(args[i]);
        if (!token.is_option || token.name != wanted)
            continue;

        if (token.has_value)
            return token.value;

        // The guess this whole family is built on: a following token that is not itself an option is
        // probably the value.
        // It might just as well be a positional, which is why none of this is for real parsing.
        if (i + 1 < args.size() && args[i + 1] != "--" && !args[i + 1].starts_with('-'))
            return args[i + 1];

        return cc::string_view();
    }

    return cc::nullopt;
}

bool nx::has_arg(cc::string_view name)
{
    return get_arg(name).has_value();
}
