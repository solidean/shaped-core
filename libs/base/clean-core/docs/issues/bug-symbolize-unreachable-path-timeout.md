---
created: 2026-11-02
refs:
  - file: libs/base/clean-core/tests/platform/symbolize-test.cc
  - test: symbolize - a module on an unreachable path resolves without waiting for the network failure
---

# An unreachable module path costs about ten seconds on a Windows laptop, at the symbolize test's bound

The test resolves eight addresses in a module whose path is `\\192.0.2.1\share\ghost.exe`, and holds the whole to under ten seconds.
On a Windows 11 laptop (Ryzen 7 260) it takes 10.01 to 10.16 seconds, twice in a row on 2026-11-02, so it fails every run there.

Measured: about ten seconds, the same with and without other symbolize tests running beside it.
Hypothesis: the default, which should not open a remote image, still pays one network timeout, perhaps DbgHelp probing the share for the module or its pdb.
The other seven resolves then hit a cache, which is why it is one timeout and not eight.
If so, the fix is in what the default hands DbgHelp, not in the bound.
