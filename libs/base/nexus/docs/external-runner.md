# External test runners: the runner report

A tool that runs nexus binaries for someone else — a result cache, a scheduler, a CI — can ask each run what its tests did.
Set `NX_EXTERNAL_RUNNER_REPORT` to a path and nexus writes one JSON file there when the run ends, beside the JUnit report.
Unset, nothing below happens and nothing is recorded.

## What it holds

Per scheduled test: its name, how it ended (`pass`, `fail`, `skip`), its duration, and every file it read, wrote or listed, with the bytes moved.
A dispatched child counts for its driver, which fails when any child fails.
Each test's wall-clock interval (`started`, `ended`, Unix epoch seconds) lets a runner that samples the process's memory
from outside tell which tests were running at each sample.
A test that allocated blocks of 1 MiB or more through clean-core carries their peak live bytes and count
(`large_peak_bytes`, `large_allocations`), from `cc::large_allocations`, which books each such block to the test that
allocated it and credits it back when it is freed, on any thread.
Small allocations, and memory taken past clean-core's resources, are not in it; a sampling runner sees the former.
Accesses no test can be blamed for — static setup, a thread no test started — are listed once under `unattributed`, and count for every test.
The run seed is there when the run was shuffled.
The format is in [`runner_report.hh`](../src/nexus/tests/export/runner_report.hh); its `version` changes with anything a reader must know about.

## How accesses are seen

nexus turns on clean-core's file-access recording (`cc::file_access`, [file_access.hh](../../clean-core/src/clean-core/platform/file_access.hh)) for the whole run.
clean-core's file streams note every path they open, a failed open included: a file found missing is an input too.
Each note is attributed to the running test through cc's ambient chain, so work on a pool worker or in an async task counts for the test that started it, and tests running side by side never mix.

## The rule for code past clean-core

Tests are meant to touch files only through clean-core.
Anything else — `std::filesystem`, `fopen`, a third-party library's own IO, the OS directly — **is not seen** unless the code declares it:

```cpp
#include <clean-core/platform/file_access.hh>
cc::file_access::note_read(path);       // read it, or looked for it
cc::file_access::note_write(path);      // wrote, created or deleted it
cc::file_access::note_list(directory);  // listed its entries; clean-core has no directory iteration, so every listing is declared
```

A declaration costs one relaxed atomic load while nothing records.
A library that opens files itself declares them where it opens them, so every caller is covered.
An undeclared read is invisible to a runner that caches on what a test read: such a test can be served a result an edit to that file should have changed.

## What a runner should make of it

The report says what a run depended on and what it produced; the policy is the runner's.
A test that writes outside its temp directory and its build directory is worth a reviewer's look, since it may change the inputs of the next run.
