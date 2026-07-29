# shaped-linter coding guidelines

shaped-linter-specific conventions, on top of the repo-wide
[coding-guidelines](../../../docs/coding-guidelines.md).
Only decisions that are not obvious from the code belong here.

## Rule tests come in two layers

Each rule is tested twice, and the two layers answer different questions.

* **Smoke tests** — `tests/rules/<rule>-test.cc`, ordinary `TEST` + `SECTION` with `run_rules_on_text`.
  This is the scratchpad you build the rule in, and where an interesting regression gets pinned.
  It is meant to stay small and pleasant to step through in a debugger — **keep it under ~200 lines**.
* **The corpus** — `tests/rules/corpus/<rule>.md`, a normal markdown file that a human reads top to bottom.
  This is where **breadth** lives: every fix shape, every scope, every look-alike that must stay quiet.
  Adding a case is adding a fenced block, not writing C++.

The split is deliberate. A rule accumulates dozens of cases, and dozens of hand-written `SECTION`s is
ceremony that buries the interesting ones. Put a case in the smoke test when *you* will want to read it
while debugging; put it in the corpus otherwise.

## The corpus format

A corpus file is prose with C++ code blocks. The checks ride on each fence's info string:

````markdown
## empty braces become an empty-brace assignment

```cpp [default-init-assignment] fix=" = {}"
struct S { int value{}; };
```

## a call-site aggregate is not a declaration

```cpp ~[default-init-assignment]
void f() { g({1, 2}); }
```
````

| Annotation | Meaning |
|------------|---------|
| `[rule-id]` | this rule must produce one finding here — repeat the annotation for N findings |
| `~[rule-id]` | this rule must produce **no** finding here |
| `fix="…"` | one replacement text the **preceding** rule produces; chain it for more |
| `hint="…"` | the same, over that rule's `hint` edits — the rewrites `--fix` does not apply |
| `path="…"` | the file name the **block** is linted as, for a rule that reads it (default `<memory>`) |

Inside a quoted value `\n` / `\t` / `\r` are the real characters, so a fix that splices in a whole line
stays spellable on the one line a fence info string gets. Any other `\x` is just `x`, which is how `\"`
and `\\` work.

`fix=` binds to the annotation in front of it, which is what associates a rewrite with a rule at all —
two rules on one block each pin their own, even when the texts are identical.

`path=` is the exception: it describes the block rather than a rule, so it stands alone and may sit
anywhere in the info string, at most once. Only the name is read, never the contents — a rule that tells
a header from a translation unit sees the extension and nothing else.

All fixes written for one rule form a **set**, and it is matched against the replacements that rule
actually produced — over every finding, every edit, duplicates merged. Naming no fix for a rule leaves
its fixes unchecked; naming one means naming them all. Because it is a set, order is irrelevant and
these two are the same pin:

```text
```cpp [r] fix=" = 1" [r] fix=" = 2"
```cpp [r] [r] fix=" = 1" fix=" = 2"
```

Finding *counts* come from the `[rule-id]` annotations alone — a `fix=` never adds one.

`hint=` works identically and is tracked as its own set, so a block may pin only its fixes, only its
hints, or both. Two things the corpus cannot express about hints, which therefore belong in the smoke
test: a **prose-only** hint (one with no edits — it contributes nothing to the set) and the **absence**
of a hint.

Rules for writing one:

* **The finding count must match the annotations exactly.** A block is linted with `all_rules()`, so a
  *second* rule firing on it is a failure until the block names it too. That is the point — it surfaces
  cross-talk between rules instead of hiding it.
* **A `cpp` block with no annotation is illustration** and is not checked, as is any non-`cpp` block.
  Use that for prose examples; the loader counts them so the skipping is not silent.
* **A malformed annotation is an error**, never "no expectations" — a typo must fail loudly.
* **Prose carries the why.** The heading names the case and appears in the test output; the sentence
  above a block says what it is demonstrating. Write it for a human first.
* **Pin corner-cuts as they behave, not as they should.** When the parser cuts a corner, the corpus
  records today's behavior so the boundary cannot move silently. Say so in the prose.

The nearest preceding heading and the fence's line number name the case, and every comparison carries
`{file}:{line}` as a chained `.context()`, so a failure says *which* block broke — plus a `.dump("rule", …)`
naming the rule when the check is per-rule.

## How the corpus reaches the test binary

`tests/rules/corpus-test.cc` is a `nx::invoke_tests` driver: it scans `SCL_CORPUS_DIR` (a path baked in
by CMake) for `*.md`, parses each with `babel::markdown`, and invokes once per file with the file's
relative path as the invocation name. So a single file is addressable:

```bash
uv run dev.py test "shaped-linter - corpus files" -c default_init_assignment.md
```

Two consequences worth knowing:

* **Adding a corpus file needs no CMake change** — only a re-run. The directory is scanned at run time.
* **`shaped-linter-test` links `babel-serializer`**, purely to read the corpus. A tool depending on a
  library is the allowed direction, and `shaped-linter-core` itself still depends on clean-core alone —
  the linter binary you ship carries no babel.

## Rules own no scope logic

A rule's `check` filters `ctx.tree.nodes` and nothing more. Everything about *where* a construct sits —
data member vs local vs namespace-scope variable, a real declaration vs a constructor's mem-initializer
vs an aggregate at a call site — is decided by the parser and handed over as `node::scope`.

Keep it that way. A rule that starts inspecting parents or re-scanning tokens to answer "is this a
member?" is a sign the parser needs the distinction instead.
