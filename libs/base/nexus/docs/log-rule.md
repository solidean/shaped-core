# nexus/log-rule — a passing test logs no warning it did not declare

**A passing test logs no warning and no error it did not declare.**
Info and below never count.

[`logs.hh`](../src/nexus/tests/logs.hh) is the authoritative description; this is the map around it.

---

## Declaring a record

From narrowest to broadest:

```cpp
nx::expect_warning("did not fit the upload ring");                // must appear in this pass, at least once
nx::expect_error("refusing connection", nx::exactly(1));          // ... exactly once
nx::expect_warning("ring full", {.domain = "my-lib", .at_least = 2}); // ... from one domain, twice or more
nx::allow_warnings("waiting on an in-flight stream");             // may appear, any number of times including none
nx::allow_errors("peer reset", "my-lib");                         // ... from one domain

TEST("stress - many uploads", nx::config::allow_logs(cc::rec::level::warning)) // the whole test waives warnings
NX_ALLOW_LOGS(cc::rec::level::warning, "sg.dx12", "clear value");              // every test in the binary
```

**A known record wants `nx::expect_*`**, which also fails when the record stops appearing.
`nx::allow_*` is for a record that may or may not happen, such as one that depends on timing.
`nx::config::allow_logs` sits on the declaration so a reader of the test list sees it, and is meant for a stress or environment test whose warnings are incidental and unpredictable.
`NX_ALLOW_LOGS` is the broadest waiver there is, so it belongs beside the driver of the tests that need it, with the reason above it.

The `expect_*` and `allow_*` calls match their own level exactly, so an error never meets an expected warning.
`allow_logs` and `NX_ALLOW_LOGS` cover everything at or below their level, so waiving errors waives warnings too.

A record that matches several declarations counts toward each of them.

## Patterns

A pattern is a text glob (`cc::glob_option::text`) matched **anywhere** in the formatted message.
`*` and `?` are the wildcards and `/` is an ordinary character.
So a line pasted out of the console matches itself, and a `?` in it still matches the `?` it came from.

The domain is optional and matched exactly; empty means any.

## Which records a declaration covers

**A declaration covers the section pass it is made in.**
One made inside a `SECTION` does not cover a sibling section, and one made above the sections covers every pass, since the code above them reruns in each.

Records from the `nexus` domain never count, since a failed check already fails.

Every pass runs under its own owner id ([`cc::rec::owner_scope`](../../clean-core/docs/systems/recording.md#an-owner-beside-the-trace)).
A record carries that id to the test from any thread the test's work reaches.
That includes a `co_await` in an `ASYNC_TEST`, a thread wrapped in `nx::attributed_to_current_test`, and a library's own `CC_RECORD_ASYNC_SCOPE`.
A message to a `cc::threaded_actor` carries its sender's owner too, so what the actor logs while handling it lands on the sending test.
The trace would not do, because every async scope mints a fresh one.

**A warning or error recorded under no test fails the run**, beside checks that ran outside any test.
No declaration reaches one: a warning under no test is a defect to fix rather than a case to declare.

A record under an owner no pass claimed — work that outlived its test — is dropped unjudged.

## When the verdict is taken

**Once, at the end of `execute_tests`, after a single `cc::rec::flush_blocking()`.**
The run keeps every warning and error as it is delivered, and judges each pass against its declarations only then.

So a declaration may follow the line that provokes it, and a verdict is final only at the end of the run.
There is no flush per test: that would be a process-wide drain once per test, which is exactly what the run avoids for its recordings too.

Each violation is filed as a failed check under the pass's leaf section and every section above it.
An undeclared record names its domain, text and site, and an unmet expectation says how many it wanted and how many it saw.

A nested `execute_tests` judges its own passes at its own end, so what an inner test logs never reaches the outer verdict.
Only the outermost run claims the records under no test, and prints its verdicts to the console.

A test failing only by this rule keeps no [recording](recording.md#a-failing-tests-recording): its bucket closes before the verdict.

## The console

**The console withholds every warning and error recorded under a pass**, outside the `nexus` domain, since it cannot know yet whether it was declared.
A declared one never prints; an undeclared one prints as the run's verdict at the end.
A record under no test prints where it happened.

A crash would otherwise lose the withheld records, so the crash handler writes them to stderr after naming the running test.
That is best effort: when their store is locked at the moment of the crash, it says so instead of waiting.

## What is outside the rule

* **`nx::config::owns_recorder`** — the run's recorder is not up while such a test runs, so there is nothing to judge.
* **`--no-recording`** — no recorder, so no rule.
