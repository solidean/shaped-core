# Stdlib Migration

Nexus dogfoods clean-core: the data model, public API, and most internals use
`cc::` types. This document tracks the **remaining `std::` usages** and what each
is waiting on, so the migration can finish as clean-core grows.

Two kinds of remaining usage:

1. **Blocked on a missing clean-core symbol** — we want a `cc::` equivalent and
   will switch as soon as it exists.
2. **Interop bridges** — glue that only exists because we still cross the
   `cc` ↔ `std` boundary. These disappear once the surrounding usage migrates,
   not because clean-core needs a new symbol.

Each site is also flagged with a short `// std::… : …` comment at the use site.

---

## Blocked on a missing clean-core symbol

| `std` in use | Where | Wanted `cc` symbol | Notes |
|---|---|---|---|
| `std::format` | [check.hh](../src/nexus/tests/check.hh) (`dump`), [check.cc](../src/nexus/tests/check.cc) (`note`/`fail_note`/`succeed_note`), [section.hh](../src/nexus/tests/section.hh) (`SECTION`), [execute.cc](../src/nexus/tests/execute.cc) (failure / exception messages) | `cc::format` | In progress. Removing it also removes the format-arg bridging below. |
| `std::unordered_map` | [execute.cc](../src/nexus/tests/execute.cc) — the executor's section tree | `cc::map` | `cc::map` is a `// TODO` stub today. |
| `std::chrono` | [execute.cc](../src/nexus/tests/execute.cc) — per-section wall-clock timing | a `cc` clock / duration | No clean-core timing API yet. |
| `std::type_index` / `typeid` (`<typeindex>`) | `nx::fuzz` — [value.hh](../src/nexus/fuzz/value.hh), [signature.hh](../src/nexus/fuzz/signature.hh), [machine.hh](../src/nexus/fuzz/machine.hh) / [machine.cc](../src/nexus/fuzz/machine.cc), [run.cc](../src/nexus/fuzz/run.cc) — runtime type identity for the type-erased value box and operation/type tables | `cc::type_id` (identity + name) | clean-core has no non-RTTI type identity. Names come from `cc::demangle_symbol(typeid(T).name())`. The single biggest gap the fuzzer hit. |
| `std::index_sequence` / `std::declval` (`<utility>`) | [signature.hh](../src/nexus/fuzz/signature.hh) — variadic operation invocation and signature deduction | a `cc` index-sequence / `declval` | Compile-time arg-pack machinery; no clean-core equivalent. |

Also wanted, even though there is no `std::` left at the call site because it was
worked around:

- **`cc::string_view::find_last_of`** — `program_name` in [run.cc](../src/nexus/run.cc)
  emulates it with `cc::max(name.rfind('/'), name.rfind('\\'))`.

---

## Interop bridges (go away when the `std` boundary does)

These are not requests for new clean-core symbols — they exist only because a
`cc::string` currently has to reach a `std` API.

- **Console output via `std::cout` / `std::cerr`.** [run.cc](../src/nexus/run.cc),
  [execute.cc](../src/nexus/tests/execute.cc), [schedule.cc](../src/nexus/tests/schedule.cc).
  `std::ostream` is intentionally **not** coming to clean-core; this migrates to
  `cc::println` once that lands. A small `as_sv()` helper (`cc::string` →
  `std::string_view`) and `os.write(s.data(), s.size())` exist purely to feed
  strings to `std::ostream` (there is no `operator<<`) and go away with it.
- **JUnit file output via `std::ofstream`.** [run.cc](../src/nexus/run.cc). Needs a
  clean-core file-write path (e.g. `cc::println` to a file sink); no concrete
  target yet.
- **`std::format` argument bridging.** Because `std::formatter<cc::string>` is
  deliberately deleted, every `cc::string` fed into `std::format` goes in as a
  `std::string_view` (or `.c_str_materialize()`): [execute.cc](../src/nexus/tests/execute.cc)
  (`as_sv`), [check.hh](../src/nexus/tests/check.hh) (`dump` label). This is moot
  once `cc::format` replaces `std::format`.
- **`std::string` map key.** [execute.cc](../src/nexus/tests/execute.cc) bridges a
  `cc::string` section name to a `std::string` key on every lookup — only because
  the map is `std::unordered_map`. Removed together with `cc::map`.
- **Console output in the fuzzer.** [fuzz/test.cc](../src/nexus/fuzz/test.cc) prints
  findings and the reproducer via `std::cerr` + the same `as_sv()` bridge — folds into
  the `cc::println` migration above.

---

## Expected to stay on `std`

- **`std::exception` (and bare `catch (...)`).** [execute.cc](../src/nexus/tests/execute.cc)
  catches whatever a test body throws so an uncaught exception becomes a reported
  failure rather than a crash. [fuzz/machine.cc](../src/nexus/fuzz/machine.cc) does the
  same around each fuzzed operation. Both run arbitrary user code that throws
  `std::exception` subclasses, so this boundary stays regardless of clean-core's surface.
- **Core type traits (`<type_traits>`).** `nx::fuzz` uses `std::decay_t`,
  `std::is_*`, `std::remove_cvref_t` for signature deduction. These are
  language-level traits with no clean-core replacement intended.

---

## Already migrated

For reference, the pieces that moved fully onto clean-core: `cc::string` /
`cc::string_view`, `cc::vector`, `cc::span`, `cc::unique_ptr` / `cc::make_unique`,
`cc::source_location`, and `cc::unique_function<void()>` for the registered test
body (a single handle — no `unique_ptr<move_only_function>` wrapper). The XML
exporters return a `cc::string` instead of writing to a `std::ostream`.

- **`cc::span::subspan`** now exists, so `is_section_allowed` in
  [execute.cc](../src/nexus/tests/execute.cc) uses `curr_section.subspan(1)` instead of
  manual `index + 1` arithmetic.
- **`cc::string` out of the assertion handler.** `cc::impl::assertion_info` now exposes
  `cc::string` fields and the handler is a `cc::unique_function`, so the fuzz engine carries
  a failed-`CC_ASSERT` message as `cc::string` through `assertion_failure`
  ([fuzz/machine.hh](../src/nexus/fuzz/machine.hh)) — no `<string>`/`<functional>` left in the
  assertion path.
- **`cc::string` text ops** (`find` / `rfind` / `subview` / `replace_all`) replaced the
  `std::string` round-trips in [schedule.cc](../src/nexus/tests/schedule.cc) — Catch2 filter
  parsing now splits with `cc::string_view`, and the `\[` → `[` normalization uses
  `cc::string::replace_all`.
