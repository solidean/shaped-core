# nx::args — declarative command-line arguments

One declaration of a command line, from which parsing, `--help`, structured diagnostics and shell completion all follow.

Nexus is the binary harness rather than only a test framework, which is why this lives here.
Every tool that already links nexus for the async pool and the crash handler gets its command line from the same place.

```cpp
#include <nexus/args/args.hh>

int main(int argc, char** argv)
{
    auto jobs = 4;
    auto verbose = false;
    auto files = cc::vector<cc::string>();

    auto args = nx::args({.name = "mytool", .description = "does the thing", .version = "0.1"});
    args.arg({"j", "jobs"}, jobs, {.desc = "how many jobs to run at once"});
    args.arg({"v", "verbose"}, verbose, "print more");
    args.positional("FILES", files, {.desc = "what to process", .min_count = 1});

    if (auto const r = args.parse(argc, argv); r.should_exit())
        return r.exit_code();

    // jobs, verbose and files are usable from here
}
```

## What is here today

The grammar below, both ways of binding — to locals and to an options struct — subcommands, and validation.
Plus ambient arguments, response files and shell completion.
Around them: error accumulation with did-you-mean, `-h` / `--help` / `--version`, groups, sections, examples, environment fallbacks, and setup validation.

Everything the library set out to do is in place.

---

## Defaults are the variable's own initializer

There is no `default_value` field, on purpose: a default that lives in two places can disagree with itself, and the help would then print something the program does not do.

```cpp
auto jobs = 4;                        // this IS the default, and this is what help prints
args.arg({"j", "jobs"}, jobs, "how many");
```

The default is formatted **once, when the argument is declared**.
Help may be asked for after a parse has already overwritten the variable, so snapshotting at declaration is the only reading that stays true.

Two escape hatches:

* `.make_default` computes it, lazily, and only when the argument was absent from a successful parse.
  It is never called to render help, so pair it with `.default_text` when the value is worth spelling out.
* `.required = true` means absence is an error.
  It also suppresses default printing, which is what makes it safe over an uninitialized variable — the one case where reading the variable before the parse would be wrong.

A bool flag prints its default only when that default is `true`.
Off-unless-given is the obvious case and says nothing; on-by-default is the surprising one.

---

## The grammar

Every line here is pinned by a test in [tests/args-grammar-test.cc](../tests/args-grammar-test.cc).

### Names

Names are always braced, and the first entry is canonical: `{"j", "jobs"}`.

* A single character is a short name (`-j`), anything longer is a long name (`--jobs`).
* A spelling that carries its own dashes is taken verbatim, which is the only way to declare a one-character *long* name: `{"x", "--x"}` gives both `-x` and `--x`.
* `nx::arg::hidden("legacy-force")` still parses and is still suggested on a typo, but never appears in help.
* Long-name lookup treats `_` and `-` as the same character, which `.exact_long_names()` turns off.
* Names are case-sensitive, and there is **no abbreviation**.
  A prefix that is unambiguous today becomes ambiguous the moment a sibling is added, so `--out` is an error that *suggests* `--output` rather than silently meaning it.

### Values

* Long: `--jobs 8` and `--jobs=8` are both accepted.
* Short: `-j 8` and fused `-j8` are both accepted.
  `-j=8` is rejected, with a message naming the two forms that work.
* Short flags cluster: `-abc`.
  A value taker consumes the rest of the token, so `-vj8` is `-v -j 8`.
* There are **no optional-value flags** — an argument either takes a value or it does not.

The one exception is a bool, and only because `=` makes it unambiguous: `--force` takes no value, while `--force=false` sets it explicitly.
Bools accept `true/false`, `yes/no`, `on/off` and `1/0`, case-insensitively.

* `.negatable` also accepts `--no-<name>`, for every long spelling.
* A `cc::vector<T>` target accumulates every occurrence; a scalar takes the last, because scripts build command lines by appending overrides.
* `.count(...)` counts occurrences instead, so `-vvv` leaves the target at 3.

### Positionals and the separator

* Positionals may be interspersed with options.
  `.stop_at_first_positional()` leaves everything after the first one untouched.
* At most one positional may be variadic (a vector target), and it may sit anywhere: the fixed slots around it are filled from both ends, so `app SRC files... DEST` means what it looks like.
* `.min_count` and `.max_count` bound a variadic's arity and change the usage line between `FILES...` and `[FILES...]`.
* `-5` is a value, not a cluster — unless a short name `5` was actually declared.
* A lone `-` is a positional, by the usual convention.

`--` ends option parsing, and its tail goes to a `.rest(...)` binding.
A `--` with nothing declared to receive it is an **error**: silently dropping what the user typed is the worst available outcome.
`args.raw()` carries the tail either way.

### Unknown arguments

An unknown argument is an error by default, carrying a suggestion when something is close enough.
`.allow_unknown(target)` collects them instead, for a wrapper that forwards them onward.

What happens to the token *after* an unknown option is genuinely ambiguous: nobody declared the flag, so nothing knows whether it takes a value.
`.allow_unknown(target, unknown_takes_value)` is where you say which reading you want.
Left false, `--mystery value` collects only `--mystery` and `value` goes on as a positional: what a runner wants, where every stray word is a filter.
Set true, a following token that does not itself start with `-` is collected too: what a wrapper wants, where the flag and its value have to stay together.
A token that does look like an option is never taken as a value either way.

`-force` — a long name typed with one dash — is diagnosed as exactly that, once, rather than as four complaints about `-o`, `-r`, `-c` and `-e`.
This only applies when the token does not also read as a real cluster, so a deliberate `-abc` still clusters.

Suggestions are never applied, only offered.
Silently correcting a mistyped flag is how a script quietly does the wrong thing for a year.

---

## What a parse returns

`--help` is neither success nor failure: the program did what was asked and should exit zero.
A bool return cannot say that, which is why `parse` returns an `nx::args_result`.

```cpp
if (auto const r = args.parse(argc, argv); r.should_exit())
    return r.exit_code();
```

| outcome | `ok()` | exit code |
|---|---|---|
| `success` | true | — |
| `help_requested`, `version_requested` | false | 0 |
| `completion_requested` | false | 0 |
| `usage_error` | false | 1, or `.usage_exit_code(n)` |
| `setup_error` | false | 70 |

Diagnostics **accumulate**: every unknown option, every failed conversion and every missing required argument come back from one run, so a user fixes their command line once instead of five times.
They are ordered by token, then by what only the end of a parse can know.

Each `nx::args_diagnostic` is structured — kind, source, token, argument name, message, suggestion — and rendered separately.
Assert on the kind; leave the wording free to improve.

---

## Setup errors are the program's fault

A duplicate short name, a negatable non-bool, two variadic positionals, a `default_command` naming something that was never declared: these are declaration bugs, not typing mistakes.

They are reported as `diagnostic_kind::setup_error`, with no suggestion, no usage line, and their own exit code, so nobody files a bug against their own command line.

The check runs inside **every** parse, in every preset — deliberately not behind `CC_ASSERT`, because a shipped release binary is exactly where this must not silently do the wrong thing.

`validate_setup` is not const: checking a `default_command` means declaring it, since a deferred subtree has no names to compare against until it is forced.

Call it from your own test suite, which is where it is cheapest to notice:

```cpp
TEST("mytool - the CLI declaration is sound")
{
    auto options = my_options();
    auto args = build_my_args(options);
    CHECK(args.validate_setup().ok());
}
```

---

## Options as a struct

The other way to bind, for a program whose options are worth a name.
A thin wrapper over the same builder rather than a second engine, so everything else on this page applies unchanged.

```cpp
struct build_options
{
    int jobs = 4;                      // the member initializers ARE the defaults
    bool verbose = false;

    static void declare_args(nx::args_builder& args, build_options& self)
    {
        args.info({.name = "build", .description = "build the project"});
        args.arg({"j", "jobs"}, self.jobs, "how many jobs to run at once");
        args.arg({"v", "verbose"}, self.verbose, "print more");
    }
};

auto const parsed = nx::parse_args<build_options>(argc, argv);
if (parsed.should_exit())
    return parsed.exit_code();

run_build(parsed.value());
```

Two tiers, in the precedence [cc::hash uses](../../clean-core/docs/customization-points.md):

1. a `nx::custom::args_trait<T>` specialization — the override tier, checked first, for a type you cannot change
2. a static member `T::declare_args(nx::args_builder&, T&)` — for a type you own

`nx::declare_args(builder, value)` is the dispatcher, and works on a builder you own too.

---

## Subcommands

A subcommand is a nested builder, not a special case, so nesting costs nothing and help at any depth is the same code path.

```cpp
auto build = build_options();
auto args = nx::args({.name = "mytool"});

args.arg({"v", "verbose"}, verbose, "print more");
args.global();                                    // ...accepted after the command name too

args.command({"build", "b"}, "build the project", [&](nx::args_builder& sub) { nx::declare_args(sub, build); });

if (auto const r = args.parse(argc, argv); r.should_exit())
    return r.exit_code();

if (args.selected_command() == "build")
    return run_build(build);
```

**Query is the primitive, not a callback.**
`selected_command()`, `command_path()` and `is_command("remote add")` let the program keep its own control flow, and the declaration lambda binds into variables the caller owns.

**Declaration is deferred.** A command's callback runs only when that command is selected, or when help or completion needs that subtree — so a tool with twenty commands pays for one on a normal run.
The contract that places on the callback, since the forcing is invisible from the call site: it must be **pure declaration, callable at any point before the parse returns, and idempotent**.

**Consequence worth knowing:** a parse validates the root plus the selected path only.
A declaration bug in a sibling surfaces on full `--help`, on completion, or on an explicit `validate_setup()`.

Other pieces:

* `.global()` applies to the argument declared just before it, and makes it reachable at any depth.
  A child's own option of the same name still wins.
* `.default_command("build")` runs one when none is named; otherwise a level with commands requires one.
  It receives **the tokens this level could not account for**, starting at the first one — an empty list only when the whole line was consumed here.
  So `app -j 8` reaches the command that declares `-j`, and `app -- --raw` hands the separator and its tail over as well.
  Its own level and the root's are both in scope while it stands in, rather than only what `global()` marked.
  That is what makes an unnamed invocation mean the same thing as a spelled-out one.
  The price is a **setup error when the root and the default command claim the same name**: which level such a token means would otherwise depend on where the root's walk happened to stop.
  Spelling the command out is always the way past it, and a name shared with a command that is *not* the default is fine.
* `help <command>` works as an alias for `<command> --help`, unless `.no_auto_help_command()`.
* Commands and positionals cannot share a level — a bare word would be ambiguous — and that is a setup error.

### Delegates

For a command this program does not own, whose tail belongs to another library's parser:

```cpp
args.delegate({"external"}, "hand off to another tool",
              [](cc::span<cc::string_view const> tail) { return other_library::main(tail); });
```

Everything after the command name passes through untouched, `--help` and `--` included, and the return value becomes the exit code.
Help lists it and says to ask it directly, rather than showing an empty option list for something it genuinely cannot introspect.

---

## Validation

A rule is an object carrying its own description, so the thing that rejects a bad value is the thing that prints the rule in help.
That is the whole reason these are not lambdas: a predicate can only say no.

```cpp
args.arg({"j", "jobs"}, jobs, {.desc = "how many", .validate = nx::arg::in_range(1, 256)});
```

renders as `-j, --jobs INT  how many [default: 4] (must be in [1, 256])`, and rejects `0` naming the same rule.

`nx::arg::` provides `in_range`, `at_least`, `at_most`, `one_of`, `non_empty` and `satisfies(description, predicate)`.
Compose with `&&`.

A bound the argument's own type cannot represent **asserts** at declaration: `in_range(1, 256)` on an `i8` would enforce `1 <= v <= 0` while help went on advertising 256.
That is a contract violation rather than a usage error — the bound is the program's own text — so it is a `CC_ASSERT` and not a diagnostic.

A factory does not need to know the bound type: it returns a spec that becomes an `arg_validator<T>` where the argument is declared, which is the only place `T` is known.

**Value rules run per occurrence**, right after conversion, so the complaint quotes the token that broke it rather than reporting the variable's final value.
On a vector target the rule is about each element.

**Cross-argument rules** go on the builder, because they are about the command line rather than any one argument:

```cpp
args.mutually_exclusive({"--quiet", "--verbose"});
args.at_least_one_of({"--input", "--stdin"});
args.requires_all("--sign", {"--key"});
args.require("width must not be less than height", [&] { return width >= height; });
```

Each states itself in the CONSTRAINTS section, so a rule cannot be enforced silently.
All of them are **skipped when anything earlier already failed**: a rule read over a half-bound command line reports something that is not true, on top of an error that is.

---

## Help

`-h` prints the short form: the argument table, nothing else.
`--help` prints the full form, which adds long descriptions, environment documentation, free-form sections and examples.

* `.group("output")` is a **mode setter** — every argument declared after it joins that section.
* `.section(title, text)`, `.example(command_line, desc)` and `.document_env(name, desc)` add the rest.
* `.env("VAR")` on an argument is the other environment feature: that one is actually read as a fallback, with precedence command line, then environment, then default.
  It shows as `[env: VAR]`.
  The value is held to the argument's `.validate` rule exactly as a typed one is: a rule that only bound what was typed would leave the variable holding something the declaration says is impossible.
  Only a binding that parses into a variable can have one; on an `action` or a `value_action` it is a setup error rather than a value read and dropped.

Width and colour are parameters, never sniffed from the process:

```cpp
args.help_text({.color = false, .width = 100});
```

That is what makes help golden-testable, and `print_help()` is the one-liner that fills them in from `cc::console` for real use.
`.no_auto_print()` stops `parse` printing anything at all.

---

## Ambient arguments

What this process was invoked with, answerable from anywhere:

```cpp
#include <nexus/args/ambient.hh>   // light: no parser comes with it

nx::test_args();       // the running test's declared arguments, empty when it declared none
nx::process_args();    // the process's own
nx::program_path();    // argv[0]
nx::program_name();    // its basename, without a directory or the platform's suffix (.exe, .js)
```

**Two sources, two names, and no fallback between them.**
A call site says which it means, because an accessor that silently switches is one that eventually hands a test the harness's own flags to parse — at the moment nobody is looking.

`nx::run` records argv, so a nexus binary answers exactly.
A binary that never went through the harness falls back to the OS — `__argv` on Windows, `/proc/self/cmdline` on Linux and Android, `_NSGetArgv` on Apple.
A platform with no answer, Emscripten included, yields an **empty list rather than an assertion**: a debug helper must never be why a program dies.

### A test's own arguments

A test or example can be given a command line, which is what makes an `EXAMPLE` demonstrate something without the reader typing anything:

```cpp
EXAMPLE("mytool/build", nx::config::args("--jobs 8 --verbose"))
{
    auto options = build_options();
    auto args = nx::args({.name = "mytool build"});
    nx::declare_args(args, options);

    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return;

    // ... demonstrate with options
}
```

Run it with something else via `uv run dev.py example mytool/build --test-args "--jobs 1"`, or `dev.py test <name> --test-args "..."`.
The CLI form **replaces** the declared line rather than adding to it — two argument lines cannot be merged into one that means anything — and it applies to every test the run selects.

Two rules worth knowing:

* `nx::test_args()` is empty for a test that declared none — it never reaches for the process's arguments.
  Ask for those by name, with `nx::process_args()`.
* They are read off the running instance through nexus's ambient chain rather than a thread-local.
  So they are correct on a pool worker, and a dispatched invocable inherits the arguments of the test that drove it.

### The undeclared accessors

```cpp
if (nx::has_arg("--trace-allocations")) { ... }
auto const budget = nx::get_arg<int>("alloc-budget").value_or(1000);
```

A debug convenience and no more.
These read a command line **nobody declared**, so they cannot know whether `--foo bar` is a flag with a value or a flag followed by a positional — they guess, and they never fail.

* `--name=value` is unambiguous; `--name value` is taken when the next token does not itself start with `-`.
* They read the **process's** arguments, always — including from inside a test, where that means the test binary's own flags rather than the test's.
* A name may be written `verbose`, `--verbose` or `-v`; all three mean the same lookup.
* Everything after a bare `--` is ignored, so an argument being passed to another program cannot switch on a debug path in this one.
* Absent and unparseable are not distinguished: a debug helper that explained itself would invite being depended on.

Worth saying out loud: these read the whole process command line, so `nx::has_arg("jobs")` in a library is true whenever `dev.py test -j8` ran.

---

## Response files

```cpp
args.enable_response_files();   // opt-in
```

`@path` is replaced by the tokens in that file, recursively, using the [tokenizer grammar](#the-tokenizer).
`@@rest` is a literal `@rest`.

**Opt-in rather than automatic**, because a program that takes user-supplied filenames would otherwise gain a file-read primitive nobody asked it for.

A file that cannot be read is an **error**, not an empty expansion — a build quietly missing half its flags is the failure this avoids.
So is a chain deeper than the cap, which is what stops a file that names itself from hanging.

Expansion **stops at a bare `--`**: past it the tokens belong to whoever receives the tail, and rewriting them would change what that program is handed.

### The tokenizer

`nx::args_tokenize` is shared by response files, the nexus runner's `--test-args`, and the line a test declares with `nx::config::args`, so all of them agree.
The rules are **ours and identical on every platform** — a response file written on Linux has to mean the same thing on Windows, and reproducing cmd.exe is not a goal:

* whitespace separates; `"..."` and `'...'` group, and may sit mid-token
* inside double quotes, a backslash escapes the next character, and the usual `n`, `t` and `r` become newline, tab and carriage return
* inside single quotes nothing is an escape, so a Windows path survives being pasted in
* `#` where a token would start runs to end of line; inside a token it is an ordinary character
* `""` is a real, empty token

---

## Shell completion

```
mytool --completion bash    # also zsh, fish, powershell
```

Generated from the same declaration as everything else, so it cannot describe a flag the program does not have.

This is where deferred subcommands pay the other way round: generating a script **forces every subtree**, because it has to know about commands this run will never touch.
A delegate contributes only its name.

The scripts complete option names, command names, and the closed value set a type publishes through `values()`.
So `--color <TAB>` offers `auto always never`, read off the same declaration the help page is built from.
`.complete` overrides that per argument: `files` and `directories` hand the value to the shell's own path completion, and `none` offers nothing even for a type that publishes a set.

bash gets one function per command and a dispatch into it, so `app build <TAB>` reaches the build level rather than the root's option list.
Deliberately not clever beyond that — a completion script that tries to be a second parser goes stale the moment the real one changes.

`.no_auto_completion()` turns the flag off.

---

## Adding a value type

Specialize `nx::custom::arg_value_trait<T>` — the same `cc::custom::`-style convention clean-core uses for [hashing and formatting](../../clean-core/docs/customization-points.md).

```cpp
template <>
struct nx::custom::arg_value_trait<mode>
{
    static bool parse(cc::string_view token, mode& out, cc::string& error)
    {
        if (token == "fast") return out = mode::fast, true;
        if (token == "slow") return out = mode::slow, true;
        error = "expected fast or slow";   // what was expected, not what failed
        return false;
    }

    static cc::string_view type_name() { return "MODE"; }

    // Optional: drives "[one of: fast, slow]" in help, and the value list in a completion script.
    static void values(cc::vector<cc::string>& out) { out.push_back("fast"); out.push_back("slow"); }
};
```

A bad value is a `false` plus a reason, never an assertion.
[clean-core's assertion policy](../../clean-core/src/clean-core/common/asserts.hh) is explicit that user input is not what assertions are for.

Formatting a **default** goes through `cc::format`, not through this trait, so a type also needs a `cc::custom::formatter` for its default to appear in help.
A type without one is still bindable; it simply shows no default.

---

## Open ends

* **Config-file value sources** (JSON, YAML, INI) are deliberately out: babel-serializer sits above nexus, so nexus cannot parse them.
  The intended seam is a `.value_source(...)` hook the application implements with whatever parser it likes.
* **Enum name tables** are per-type traits today.
  A general `cc::custom::enum_names<E>` in clean-core is the eventual right home, once a second caller exists — `cc::custom::enum_traits` is flags-only and cannot be extended for it.
* **A path vocabulary type** does not exist in the repo, so file-ish arguments are strings and completion needs an explicit `.complete` hint.

## Related

* [cheat-sheet.md](../cheat-sheet.md) — the API at a glance.
* [docs/_index.md](_index.md) — the rest of nexus's documentation.
* [docs/coding-guidelines.md](../../../../docs/coding-guidelines.md) — the conventions this follows.
