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
| `std::string` (`find` / `substr` / `replace`) | [schedule.cc](../src/nexus/tests/schedule.cc) — Catch2 filter parsing & the `\[` → `[` normalization | `cc::string` text ops | `cc::string_view` already has `find`/`rfind`/`subview`/`contains` (used for the read-only matching). The gap is **in-place `replace`** and `find`/`substr` on the owning `cc::string`. |
| `std::span::subspan` | [execute.cc](../src/nexus/tests/execute.cc) — `is_section_allowed` (replaced with index arithmetic) | `cc::span::subspan` | Listed as a TODO in [span.hh](../../clean-core/src/clean-core/container/span.hh). Design under review (possible footgun), hence the manual indexing for now. |
| `std::chrono` | [execute.cc](../src/nexus/tests/execute.cc) — per-section wall-clock timing | a `cc` clock / duration | No clean-core timing API yet. |

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

---

## Expected to stay on `std`

- **`std::exception` (and bare `catch (...)`).** [execute.cc](../src/nexus/tests/execute.cc)
  catches whatever a test body throws so an uncaught exception becomes a reported
  failure rather than a crash. Test bodies are arbitrary user code that throws
  `std::exception` subclasses, so this boundary stays regardless of clean-core's
  surface.

---

## Already migrated

For reference, the pieces that moved fully onto clean-core: `cc::string` /
`cc::string_view`, `cc::vector`, `cc::span`, `cc::unique_ptr` / `cc::make_unique`,
`cc::source_location`, and `cc::unique_function<void()>` for the registered test
body (a single handle — no `unique_ptr<move_only_function>` wrapper). The XML
exporters return a `cc::string` instead of writing to a `std::ostream`.
