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

## Which code each test ran, sampled

Set `NX_EXTERNAL_RUNNER_SAMPLE_HZ` to a rate as well and the report says which code each test ran, as a sampling profiler saw it.
clean-core's sampler (`cc::rec` sampling, [sampling.hh](../../clean-core/src/clean-core/record/sampling.hh)) walks every thread the recorder knows at that rate per thread, whole stacks,
and hands each sample to nexus instead of the recording.
nexus installs a fresh owner (`cc::rec::owner_scope`) around every pass of every test; the sampler reads the owner the thread was under while it holds the thread suspended,
so a sample on a pool worker or in an async task counts for the test that started the work, and tests side by side never mix.
A dispatched child counts for its driver; samples under no test, or under a nested run's tests, are `unattributed`.

Per test the report then carries `samples`, how many caught it, and `sampled`, the code addresses they hit:
per module its index into the top-level `modules` (path and build identity), then the sorted offsets into it.
A caller's frame is its return address minus one, so every offset lies inside the instruction that ran or called; the build's debug info resolves them, inlined frames included.
`body` is where the test's body starts, when the build keeps code addresses for source sites (below `SC_SOURCE_SITES=FULL`).
The top-level `sampling` says the rate and how many samples were taken and lost.

A sample is a statistical witness: code it names ran in that test, and code it does not name may still have run.
A test of a few milliseconds is caught by few samples or none, so a runner unions the sets over many runs.
Measured on this repo's `clean-core-test` and `nexus-test` at 1 kHz, the run took no measurably longer (within the noise of a few percent); without a recorder (`--no-recording`) nothing is sampled.

## Which functions each test entered, exactly

In a coverage build (clang's `-fprofile-instr-generate`, as the `*-coverage` presets build), set `NX_EXTERNAL_RUNNER_COVERAGE=1` as well and the report says exactly which functions each test entered.
The profile counters are per process, so nexus then runs every test alone (`test_schedule_config::one_at_a_time`), own_pool phases included; work inside a test still fans out over `--jobs`.
As each top-level test begins and ends, nexus reads the counters of every function the profile runtime knows and resets them ([runner_coverage.cc](../src/nexus/tests/export/runner_coverage.cc)),
so a test's set is what ran between its two boundaries: its dispatched children and a nested run's tests count for it.

The top-level `coverage.functions` lists the profile's function name hashes, once per run, and each test's `covered` lists indices into it.
`unattributed` holds what ran outside every test, static setup included.
A hash is the profile's own, the low 64 bits of the MD5 of the function's profile name, so the build's coverage mapping names each one:
`llvm-cov export -format=lcov` on the profile the run writes at exit lists every function by that name, with its file and line.
A child process is not counted, and work a test left running counts for whichever test runs while it does.
Asked of a build that does not count coverage, the report has no `coverage` and the run is scheduled as usual.

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
