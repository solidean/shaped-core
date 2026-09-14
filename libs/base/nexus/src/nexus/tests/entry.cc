#include "entry.hh"

#include <clean-core/string/format.hh>
#include <nexus/tests/config.hh>
#include <nexus/tests/registry.hh>

using namespace cc::primitive_defines;

namespace
{
using bucket = nx::config::test_bucket;

[[nodiscard]] bool is_entry_bucket(bucket b)
{
    return b == bucket::app || b == bucket::command;
}

/// A token that hands the line to nexus's own parser, spelled bare or with `=value`.
[[nodiscard]] bool is_nexus_selector(cc::string_view token)
{
    for (auto const selector : {"--tests", "--examples", "--benchmarks", "--pgo-benchmarks", "--manual", "--apps",
                                "--commands", "--list-tests", "--list-tests-json", "--reporter"})
    {
        auto const s = cc::string_view(selector);
        if (token == s || (token.starts_with(s) && token.size() > s.size() && token[s.size()] == '='))
            return true;
    }
    return false;
}

/// Up to `limit` names, then how many were left out.
void append_names(cc::string& out, cc::string_view label, cc::vector<cc::string_view> const& names, isize limit)
{
    if (names.empty())
        return;

    out.appendf("  {:<12}", label);
    for (isize i = 0; i < names.size() && i < limit; ++i)
        out.appendf("{}{}", i == 0 ? "" : ", ", names[i]);
    if (names.size() > limit)
        out.appendf(" … and {} more (--list-tests-json - for all)", names.size() - limit);
    out += "\n";
}
} // namespace

cc::string nx::impl::check_default_entries(test_registry const& registry)
{
    auto problems = cc::string();
    auto defaults = cc::vector<test_declaration const*>();
    for (auto const& decl : registry.declarations)
    {
        if (!decl.test_config.default_entry)
            continue;
        if (!is_entry_bucket(decl.test_config.bucket))
            problems.appendf("\"{}\" is marked default_entry, but only an APP or a COMMAND can be a binary's default\n",
                             decl.name);
        else
            defaults.push_back(&decl);
    }

    if (defaults.size() > 1)
    {
        problems
            += "more than one app or command is marked default_entry, so a run with nothing selected cannot choose:";
        for (auto const* decl : defaults)
            problems.appendf(" \"{}\"", decl->name);
        problems += "\n";
    }
    return problems;
}

nx::impl::entry_route nx::impl::route_command_line(test_registry const& registry, cc::span<cc::string_view const> args)
{
    auto route = entry_route{};

    auto const find_entry = [&](cc::string_view name) -> test_declaration const*
    {
        for (auto const& decl : registry.declarations)
            if (is_entry_bucket(decl.test_config.bucket) && cc::string_view(decl.name) == name)
                return &decl;
        return nullptr;
    };
    auto const rest_after = [&](isize first)
    {
        auto rest = cc::vector<cc::string>();
        for (auto i = first; i < args.size(); ++i)
            rest.push_back(cc::string(args[i]));
        return rest;
    };

    // Name first: an app or command named by the first token takes the rest of the line.
    if (!args.empty() && !args[0].starts_with('-'))
        if (auto const* const entry = find_entry(args[0]); entry != nullptr)
        {
            route.kind = entry_route_kind::entry;
            route.entry = entry;
            route.entry_args = rest_after(1);
            return route;
        }

    // A nexus selector anywhere makes the line nexus's.
    for (auto const token : args)
        if (is_nexus_selector(token))
            return route; // tests

    // A test named exactly, as a direct name has always selected across buckets.
    if (!args.empty() && !args[0].starts_with('-'))
    {
        for (auto const& decl : registry.declarations)
            if (!decl.is_invocable() && cc::string_view(decl.name) == args[0])
                return route;
        for (auto const& alias : registry.aliases)
            if (cc::string_view(alias.name) == args[0])
                return route;
    }

    // The default takes the whole line — including a --help, which is then its own help.
    for (auto const& decl : registry.declarations)
        if (decl.test_config.default_entry && is_entry_bucket(decl.test_config.bucket))
        {
            route.kind = entry_route_kind::entry;
            route.entry = &decl;
            route.entry_args = rest_after(0);
            return route;
        }

    if (args.empty())
    {
        route.kind = entry_route_kind::overview;
        return route;
    }

    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h"))
        return route; // nexus's own help, since there is no default to own it

    route.kind = entry_route_kind::error;
    route.message = cc::format("nothing here is called \"{}\": it names no app, command or test in this binary; to "
                               "filter tests, run with --tests \"{}\"",
                               args[0], args[0]);
    return route;
}

cc::string nx::impl::render_overview(test_registry const& registry, cc::string_view program)
{
    constexpr auto name_limit = isize(8);

    auto apps = cc::vector<cc::string_view>();
    auto commands = cc::vector<cc::string_view>();
    auto examples = cc::vector<cc::string_view>();
    auto benchmarks = cc::vector<cc::string_view>();
    auto pgo_benchmarks = cc::vector<cc::string_view>();
    auto tests = isize(0);
    auto manual = isize(0);

    for (auto const& decl : registry.declarations)
    {
        if (decl.is_invocable())
            continue;
        switch (decl.test_config.bucket)
        {
        case bucket::normal:
            ++tests;
            break;
        case bucket::manual:
            ++manual;
            break;
        case bucket::pgo_benchmark:
            pgo_benchmarks.push_back(decl.name);
            break;
        case bucket::benchmark:
            benchmarks.push_back(decl.name);
            break;
        case bucket::example:
            examples.push_back(decl.name);
            break;
        case bucket::app:
            apps.push_back(decl.name);
            break;
        case bucket::command:
            commands.push_back(decl.name);
            break;
        }
    }

    auto out
        = cc::format("{} — {} command(s), {} app(s), {} example(s), {} benchmark(s), {} test(s)\n\n", program,
                     commands.size(), apps.size(), examples.size(), benchmarks.size() + pgo_benchmarks.size(), tests);
    append_names(out, "commands", commands, name_limit);
    append_names(out, "apps", apps, name_limit);
    append_names(out, "examples", examples, name_limit);
    append_names(out, "benchmarks", benchmarks, name_limit);
    append_names(out, "pgo", pgo_benchmarks, name_limit);
    if (tests > 0)
        out.appendf("  {:<12}{} (run with --tests, filter with --tests \"<name>\")\n", "tests", tests);
    if (manual > 0)
        out.appendf("  {:<12}{} (run with --manual)\n", "manual", manual);

    out += "\n";
    if (!commands.empty() || !apps.empty() || !examples.empty() || !benchmarks.empty())
        out.appendf("  run one:    {} <name> [args...]\n", program);
    out.appendf("  all flags:  {} --help\n", program);
    return out;
}
