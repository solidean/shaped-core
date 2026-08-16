# Checks off the test's own thread

A `CHECK` has to know which test it belongs to.
That used to be a `thread_local` stack, which answered correctly for exactly one thread and silently answered *nothing* for every other one — a check on a worker was dropped, uncounted and unfailable.

Attribution now rides the work itself, through cc::async's [ambient context](../../clean-core/docs/systems/async.md#ambient-context).
This page is what that means when you write a test that does work on more than one thread.
The API is in the [cheat-sheet](../cheat-sheet.md#checks-off-the-tests-own-thread).

## The rule

> **A check that cannot be attributed to a test fails the run, whether it passed or not.**

It is printed the moment it happens and summarized at the end as `N check(s) ran outside any test context`, and the run's exit code says so even when every test is green.

That is deliberate, and it is the whole point of the change.
A check nobody counts is worse than a missing one: it reads as coverage, runs green forever, and the day it starts failing nothing notices.

## Where attribution comes from

**Work driven by `cc::async` needs nothing from you.**
A node records which test it belongs to when it is queued, parked or yielded, and `poll()` installs that back for the duration.
So a check inside a frame is billed to the test that scheduled it, on whatever worker happens to run it.

This is why the ambient is the *only* source of truth here, rather than a fallback for when the thread stack is empty.
A thread inside `blocking_get` does not hand its graph over and wait; it borrows a slot and **steals like any worker**.
So a test's own thread runs *other* tests' nodes, with its own test sitting on its own stack.
Reading that stack would bill those checks to the wrong test — silently, and only under load, which is the worst way to be wrong.

**A thread you start yourself carries nothing**, so say what it belongs to:

```cpp
std::thread t(nx::attributed_to_current_test([&] { CHECK(worker_saw_it); }));
```

For a thread that is already running — a callback arriving on a library's own thread — capture on the test's thread and install on the other one:

```cpp
auto captured = nx::capture_current_test();       // on the test's thread
// ... on the other thread:
nx::test_thread_scope const scope(captured);
```

## What an attributed off-thread check does and does not get

It is **counted**, its failure **fails the test**, and its message reaches the report.
Three things stay with the test's own thread:

* **Sections.** The body is replayed once per section path, which only the test's thread does — so a `SECTION` opened elsewhere is a recorded failure rather than something to serialize.
  Off-thread checks are attributed to the test's root section.
* **Aborting.** `REQUIRE` and `SKIP` abort by throwing.
  Inside an async frame that is fine: cc::async contains the throw and fails the node on its error channel, which is exactly the "stop this computation" the caller asked for.
  On a bare thread there is nothing between the throw and the thread function, so it would terminate the process — there it degrades to a recorded failure and execution continues.
* **Ordering.** Off-thread checks merge into the test's totals when the test ends, not as they arrive.

## Work that outlives its test

**A test that leaves async work running fails, by name.**
The work still carries that test's context, so it would run during a *later* test and report against one that has already finished.
That is precisely the interference parallel execution exists to find.

If it does run afterwards anyway, its checks become orphans naming the test they came from, never mis-billed to whatever was running at the time.

## Deliberately not a lock

The test thread's own path is unchanged: plain counters, no atomics, no lock, full section awareness.
Only the off-thread path pays for synchronization, and it pays into a separate spill that is merged once.
Making the shared state thread-safe instead would have put a lock on every `CHECK` in the repo to serve the few that need it.
