#pragma once

#include <nexus/fwd.hh>

namespace nx
{
struct app_info;    // the name, description and version a help page is built around (args/options.hh)
class args_builder; // one command line, declared (args/builder.hh)

struct args_diagnostic;     // one thing wrong with a command line, or with its declaration (args/diagnostic.hh)
struct args_render_options; // how diagnostics and help are turned into text (args/diagnostic.hh)
class args_result;          // what a parse concluded, and what the process should do about it (args/diagnostic.hh)

template <class T>
struct arg_options; // everything about one argument except its names and its target (args/options.hh)

template <class T>
class parsed; // the by-value counterpart of args_result, carrying the struct it filled (args/args.hh)

struct arg_source; // where a token came from, which splicing makes worth recording (args/diagnostic.hh)

enum class diagnostic_kind; // which way a command line was wrong (args/diagnostic.hh)
enum class arg_origin;      // which source a token came from (args/diagnostic.hh)
enum class args_outcome;    // what a parse concluded, beyond pass or fail (args/diagnostic.hh)
enum class complete_hint;   // what a shell should offer for an argument's value (args/options.hh)

} // namespace nx

namespace nx::arg
{
struct name_spec; // one declared name, and whether help is allowed to show it (args/options.hh)
} // namespace nx::arg

namespace nx::custom
{
template <class T>
struct arg_value_trait; // how one value type is parsed and described (args/value.hh)

template <class T>
struct args_trait; // how one options struct declares itself (args/args.hh)
} // namespace nx::custom

namespace nx::impl
{
struct binding;        // one declared argument, with its type erased (args/impl/binding.hh)
struct common_options; // the T-independent half of arg_options (args/impl/binding.hh)
struct parse_engine;   // the token grammar (args/impl/parse_engine.hh)
struct help_renderer;  // the help page (args/impl/help_render.hh)
struct setup_checker;  // what a declaration must satisfy (args/impl/setup_check.hh)
} // namespace nx::impl
