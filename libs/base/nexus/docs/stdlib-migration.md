# Stdlib Migration

Nexus dogfoods clean-core: the data model, the public API and most internals use `cc::` types.
This document tracks the **remaining `std::` usages** and what each is waiting on, so the migration can finish as clean-core grows.

Two kinds of remaining usage:

1. **Blocked on a missing clean-core symbol** — a `cc::` equivalent is wanted, and the site switches as soon as it exists.
2. **Interop bridges** — glue that exists only because the `cc` ↔ `std` boundary is still crossed here.
   These disappear when the surrounding usage migrates, not when clean-core grows a symbol.

Each site is also flagged with a short `// std::… : …` comment at the use site.

---

## Blocked on a missing clean-core symbol

| `std` in use | Where | Wanted `cc` symbol | Notes |
|---|---|---|---|
| `std::unordered_map` | [execute.cc](../src/nexus/tests/execute.cc) — the executor's section tree | `cc::map` | `cc::map` is a `// TODO` stub today. |
| `std::chrono` | [execute.cc](../src/nexus/tests/execute.cc) — per-section wall-clock timing | a `cc` clock / duration | No clean-core timing API yet. |
| `std::type_index` / `typeid` (`<typeindex>`) | [typed_value.hh](../src/nexus/tests/typed_value.hh) (the shared type-erased value box), `nx::fuzz` — [signature.hh](../src/nexus/fuzz/signature.hh), [machine.hh](../src/nexus/fuzz/machine.hh) / [machine.cc](../src/nexus/fuzz/machine.cc), [run.cc](../src/nexus/fuzz/run.cc) — runtime type identity for the type-erased value box and operation/type tables | `cc::type_id` (identity + name) | clean-core has no non-RTTI type identity. Names come from `cc::demangle_symbol(typeid(T).name())`. The single biggest gap the fuzzer hit. |
| `std::index_sequence` / `std::declval` (`<utility>`) | [signature.hh](../src/nexus/fuzz/signature.hh) — variadic operation invocation and signature deduction | a `cc` index-sequence / `declval` | Compile-time arg-pack machinery; no clean-core equivalent. |

Also wanted, though the call site has no `std::` left because it was worked around:

- **`cc::string_view::find_last_of`** — `program_name` in [run.cc](../src/nexus/run.cc)
  emulates it with `cc::max(name.rfind('/'), name.rfind('\\'))`.

---

## Interop bridges (go away when the `std` boundary does)

These are not requests for new clean-core symbols — they exist only because a `cc::string` currently has to reach a `std` API.

- **Console output via `std::cout` / `std::cerr`.**
  [execute.cc](../src/nexus/tests/execute.cc), [schedule.cc](../src/nexus/tests/schedule.cc).
  `std::ostream` is intentionally **not** coming to clean-core, so each of these moves to
  `cc::print` / `cc::println` the way [run.cc](../src/nexus/run.cc) already has.
  While a file still uses `std::ostream` it needs the `as_sv()` bridge (`cc::string` →
  `std::string_view`) and `os.write(s.data(), s.size())`, because there is no `operator<<`
  for `cc::string`; both go away with the last stream.
  Mixing the two is safe in the meantime — nobody calls `sync_with_stdio(false)`, so
  `std::cout` and `cc::print` stay in order.
- **`std::string` map key.** [execute.cc](../src/nexus/tests/execute.cc) bridges a
  `cc::string` section name to a `std::string` key on every lookup — only because
  the map is `std::unordered_map`. Removed together with `cc::map`.
- **Console output in the fuzzer.** [fuzz/test.cc](../src/nexus/fuzz/test.cc) prints
  findings and the reproducer via `std::cerr` + the same `as_sv()` bridge — folds into
  the `cc::println` migration above.

`nx::run` itself is done: it prints through `cc::print` / `cc::println` / `cc::eprintln`
and includes no `<iostream>`.

---

## Expected to stay on `std`

- **`std::exception` (and bare `catch (...)`).** [execute.cc](../src/nexus/tests/execute.cc) catches whatever a test body throws.
  An uncaught exception becomes a reported failure rather than a crash, and [fuzz/machine.cc](../src/nexus/fuzz/machine.cc) does the same around each fuzzed operation.
  Both run arbitrary user code that throws `std::exception` subclasses, so this boundary stays whatever clean-core grows.
- **Core type traits (`<type_traits>`).** `nx::fuzz` uses `std::decay_t`, `std::is_*` and `std::remove_cvref_t` for signature deduction.
  These are language-level traits with no clean-core replacement intended.

---

## Already migrated

The pieces that moved fully onto clean-core: `cc::string` / `cc::string_view`, `cc::vector`, `cc::span`, `cc::unique_ptr` / `cc::make_unique`, and `cc::source_location`.
The registered test body is a single `cc::unique_function<void()>` handle, with no `unique_ptr<move_only_function>` wrapper.
The XML exporters return a `cc::string` instead of writing to a `std::ostream`.

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
- **`cc::format` replaced `std::format`** in `dump` ([check.hh](../src/nexus/tests/check.hh)),
  `note`/`fail_note`/`succeed_note` ([check.cc](../src/nexus/tests/check.cc)), `SECTION`
  ([section.hh](../src/nexus/tests/section.hh)), and the failure / exception messages
  ([execute.cc](../src/nexus/tests/execute.cc)). `cc::format` formats `cc::string` /
  `cc::string_view` directly, so the format-arg `std::string_view` bridging is gone (the
  remaining `as_sv` exists only for `std::cout`).
