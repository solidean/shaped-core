# Configuring shaped-linter — `.shaped-lint.yml`

Some rules cannot be written down once for the whole tree, because the answer differs per library.
Which standard headers a library may include is the first of them.
clean-core reaches for `<type_traits>` and nothing else, shaped-graphics deliberately owns `std::shared_ptr`, and a test may reach for `<vector>` to compare `cc::` against it.

`.shaped-lint.yml` is where a library states its own policy, next to its readme.
It is the machine-checked half of what [coding-guidelines](../../../docs/coding-guidelines.md) states in prose.

Any directory may carry one; nothing is special-cased, and the repo root carries the shared policy.

## The format

A strict YAML subset, hand-parsed on clean-core.
`shaped-linter-core` links clean-core alone — deliberately, so the shipped binary carries no babel — which is the same reason `markdown_scanner` is not `babel::markdown`.

```yaml
# Comments run to the end of the line.

rules:
  - kind: allow-include
    value: <type_traits>
    reason: thin wrappers around compiler intrinsics, no value in re-wrapping

  - kind: deny-include
    value: [<cstddef>, <cstdint>]
    reason: use clean-core/fwd.hh
    exclude-files: src/clean-core/fwd.hh
```

What the parser accepts:

* `#` to the end of the line is a comment, unless it sits inside quotes or is glued to a word (`c#d`).
* The top level is a mapping of `key: value` entries, and `rules` is the only key today.
* A value is a scalar (the rest of the line, trimmed, optionally `'`/`"` quoted), a flow list `[a, b, c]`, or — when the line ends after the `:` — an indented block below it.
* A block is either `- ` items or a nested mapping, and an item may itself be a mapping.
* Only a `:` at the end of the line or followed by a space ends a key, so a value spells `cc::atomic` without quoting.

Indentation is **spaces only — a tab is an error**, never a silent eight columns.
Siblings align exactly and a nested block is strictly deeper; the width is otherwise free, though everything we write uses two.

Anchors, tags, multi-line scalars, flow mappings and a block list at its key's own indentation are all errors rather than quiet reinterpretations.
So is an unknown key, an unknown `kind` and an unknown section: a misspelled `rules` would otherwise disable every blessing in the file without a word.

A config that fails to parse fails the whole run.
The policy it carries is unknown, so every verdict taken against it would be a guess.

## Rule entries

| Field | Required | Meaning |
|---|---|---|
| `kind` | yes | `allow-include` or `deny-include` |
| `value` | yes | the include as written (`<atomic>`), or a list of them; each is a glob, so `<d3d12*.h>` covers a family |
| `reason` | yes | printed with the finding — for a deny it names the replacement |
| `files` | no | glob or list of globs; the entry applies only to these |
| `exclude-files` | no | glob or list of globs; the entry does not apply to these |

The `reason` is mandatory because it is the whole point of the file.
`<mutex> is not blessed here` teaches a reader nothing; `use clean-core/thread/mutex.hh` teaches them what to do.

**Globs** understand `?` (one character), `*` (a run of them) and `**` (a run that crosses `/`); a trailing `/` is shorthand for the subtree.
The separator after a `**` is optional, so `src/**/x.hh` also matches `src/x.hh`.

**Globs match the path relative to the config's own directory.**
That is what lets clean-core's file say `src/clean-core/thread/atomic.hh` while a root entry says `libs/base/clean-core/**` — and it is why an entry never reaches a file outside its own subtree.

Include spellings are matched case-insensitively; paths are not.
The Windows SDK is reached as both `<dbghelp.h>` and `<DbgHelp.h>` in this tree, and neither spelling is more correct than the other.

## Merging, and who wins

Every `.shaped-lint.yml` from the file's own directory up to the filesystem root applies, merged **root-first, nearest-last**.
A nearer config *extends* its ancestors; nothing replaces them.

Evaluating one include keeps the **last entry that matches** both the include and the file, and that entry decides:

| Last match | Verdict |
|---|---|
| `allow-include` | silent |
| `deny-include` | a finding carrying its `reason` |
| nothing matched | a finding — the default is deny |

So `deny-include` is not redundant with the default.
It exists to carry a pointer at the right header, which "not blessed" cannot.

The shape this produces is the one the repo uses: the root denies `<atomic>` and points at `cc::atomic`, and clean-core's own config re-opens it for the single file that *is* the seam.

```yaml
# libs/base/clean-core/.shaped-lint.yml
rules:
  - kind: allow-include
    value: <atomic>
    reason: cc::atomic IS std::atomic when CC_HAS_THREADS — this file is the seam
    files: src/clean-core/thread/atomic.hh
```

A library never edits the root to make room for itself.

## The generated baseline block

`uv run dev.py lint bless-includes --write` fills the block at the end of each config with an `allow-include` for every include the tree below it still needs.
It exists to get a config started: what a library includes today is written down, and the entries then get curated into scoped, argued ones.

**No config in the repo carries a block today** — they were all curated away, which is the state to keep them in.
A library-wide `allow-include` with `reason: baseline` says only that the library exists; the entry that earns its place names *which* files may include the header and *why*.
Run the generator when a new library appears, or after a sweep, and read its output as a to-do list rather than an answer.

Everything between the markers belongs to the generator and is rewritten on every run.
Everything above them is a human's and is never touched — so **curating means moving an entry out of the block**, or deleting it and fixing the include.
A re-run reproduces the block byte for byte, which is what makes the next diff exactly the decisions someone made.

The baseline is deliberately coarse: it blesses a header for the whole library, with `reason: baseline`, and never guesses a `files:` scope.
Narrowing one to the tests, or to the one file that is the seam, is the curation.

`bless-includes` fills configs in; it does not decide where they belong, so a file with no `.shaped-lint.yml` above it is skipped.

## A file with no config above it is unchecked

An empty merged policy means "nothing was said here", not "deny everything".
That is what keeps a corpus block, a `run_rules_on_text` snippet and an as-yet-unconfigured directory out of the gate, and it is what lets the configs adopt one library at a time.

## How it reaches a rule

`config_resolver` walks up from each file, reads what it finds, and caches per directory — a 200-file batch reads each config once.
The merged `lint_config` is resolved once per file and handed to the engine, which puts it on `lint_context::config`.

**This is the only part of the pipeline that touches the filesystem.** `run_rules` is given a config rather than finding one, so a test is hermetic wherever it runs from, and a rule never does IO.

A rule that reads config starts by asking whether there is any:

```cpp
if (!ctx.config.checks_includes())
    return;
```
