#include "describe.hh"

#include <nexus/args/builder.hh>
#include <nexus/args/impl/parse_engine.hh>

namespace
{
nx::impl::described_option describe_option(nx::impl::binding const& b)
{
    auto out = nx::impl::described_option();
    out.desc = b.desc;
    out.metavar = b.metavar;
    out.takes_value = b.takes_value;
    out.complete = b.complete;

    for (auto const& n : b.names)
        if (!n.hidden)
            out.spellings.push_back(n.display());

    if (b.enumerate_values_fn != nullptr && b.takes_value)
        b.enumerate_values_fn(out.values);

    return out;
}
} // namespace

nx::impl::described_command nx::impl::describer::run(args_builder& builder)
{
    auto out = described_command();
    out.name = cc::string(builder._info.name);
    out.desc = cc::string(builder._info.description);

    for (auto const& b : builder._bindings)
    {
        if (b.hidden || b.kind != binding_kind::option)
            continue;

        out.options.push_back(describe_option(b));
    }

    // Inherited globals are completable at this depth too, which is the whole point of marking them.
    for (auto* up = builder._parent; up != nullptr; up = up->_parent)
        for (auto const& b : up->_bindings)
            if (!b.hidden && b.kind == binding_kind::option && b.is_global)
                out.options.push_back(describe_option(b));

    for (auto& node : builder._commands)
    {
        if (node.hidden)
            continue;

        if (node.is_delegate())
        {
            out.commands.push_back({.name = node.canonical, .desc = node.desc, .opaque = true});
            continue;
        }

        // Forcing every subtree is the cost of describing a tree at all: a completion script has to know
        // about commands this run will never touch.
        auto& child = parse_engine::force_declare(builder, node);
        auto described = run(child);
        described.name = node.canonical;
        described.desc = node.desc;
        out.commands.push_back(cc::move(described));
    }

    return out;
}
