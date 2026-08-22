#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/fwd.hh>
#include <nexus/args/options.hh>

// The declaration, flattened into data.
//
// One shape that both completion and any future machine-readable dump read, so a shell script and a
// documentation generator can never disagree about what the program accepts.
//
// Producing it FORCES every deferred subcommand, since a description that omits the commands nobody has
// run yet would be worse than useless.

namespace nx::impl
{
struct described_option
{
    cc::vector<cc::string> spellings; // "--jobs", "-j", visible ones only
    cc::string desc;
    cc::string metavar;
    bool takes_value = false;
    complete_hint complete = complete_hint::automatic;
    cc::vector<cc::string> values; // the closed set, when the type publishes one
};

struct described_command
{
    cc::string name;
    cc::string desc;
    bool opaque = false; // a delegate: its name is all we can honestly offer

    cc::vector<described_option> options;
    cc::vector<described_command> commands;
};

struct describer
{
    /// The whole tree, rooted at `builder`.
    static described_command run(args_builder& builder);
};

} // namespace nx::impl
