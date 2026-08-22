#pragma once

#include <nexus/args/builder.hh>
#include <nexus/args/diagnostic.hh>
#include <nexus/args/options.hh>
#include <nexus/args/parse_args.hh>
#include <nexus/args/validation.hh>
#include <nexus/args/value.hh>

// =========================================================================================================
// nx::args — one declaration of a command line, from which parsing, help, diagnostics and completion all
// follow.
//
//     auto jobs = 4;
//     auto files = cc::vector<cc::string>();
//
//     auto args = nx::args({.name = "mytool", .description = "does the thing", .version = "0.1"});
//     args.arg({"j", "jobs"}, jobs, {.desc = "how many jobs to run at once"});
//     args.positional("FILES", files, {.desc = "what to process", .min_count = 1});
//
//     if (auto const r = args.parse(argc, argv); r.should_exit())
//         return r.exit_code();
//
// The defaults are the variables' own initializers, so help prints what the program will actually do.
// `--help` is neither success nor failure, which is why parse returns a result rather than a bool.
//
// libs/base/nexus/docs/args.md is the reference, and carries the token grammar as a spec.
// =========================================================================================================
