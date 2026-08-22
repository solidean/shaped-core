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

The core: the grammar below, bind-to-locals, error accumulation with did-you-mean, `-h` / `--help` / `--version`, groups, sections, examples, environment fallbacks, and setup validation.

Still to land, in this order: subcommands, validation, the options-struct protocol, ambient args, response files, and completion generation.

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
| `usage_error` | false | 1, or `.usage_exit_code(n)` |
| `setup_error` | false | 70 |

Diagnostics **accumulate**: every unknown option, every failed conversion and every missing required argument come back from one run, so a user fixes their command line once instead of five times.
They are ordered by token, then by what only the end of a parse can know.

Each `nx::args_diagnostic` is structured — kind, source, token, argument name, message, suggestion — and rendered separately.
Assert on the kind; leave the wording free to improve.

---

## Setup errors are the program's fault

A duplicate short name, a negatable non-bool, two variadic positionals: these are declaration bugs, not typing mistakes.

They are reported as `diagnostic_kind::setup_error`, with no suggestion, no usage line, and their own exit code, so nobody files a bug against their own command line.

The check runs inside **every** parse, in every preset — deliberately not behind `CC_ASSERT`, because a shipped release binary is exactly where this must not silently do the wrong thing.

Call `args.validate_setup()` from your own test suite, which is where it is cheapest to notice:

```cpp
TEST("mytool - the CLI declaration is sound")
{
    auto options = my_options();
    auto args = build_my_args(options);
    CHECK(args.validate_setup().ok());
}
```

---

## Help

`-h` prints the short form: the argument table, nothing else.
`--help` prints the full form, which adds long descriptions, environment documentation, free-form sections and examples.

* `.group("output")` is a **mode setter** — every argument declared after it joins that section.
* `.section(title, text)`, `.example(command_line, desc)` and `.document_env(name, desc)` add the rest.
* `.env("VAR")` on an argument is the other environment feature: that one is actually read as a fallback, with precedence command line, then environment, then default.
  It shows as `[env: VAR]`.

Width and colour are parameters, never sniffed from the process:

```cpp
args.help_text({.color = false, .width = 100});
```

That is what makes help golden-testable, and `print_help()` is the one-liner that fills them in from `cc::console` for real use.
`.no_auto_print()` stops `parse` printing anything at all.

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

    // Optional: drives "[one of: fast, slow]" in help, and completion later.
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
