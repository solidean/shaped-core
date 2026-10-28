---
created: 2026-10-28
refs:
  - file: docs/bugs-external/warp-raytracing-wrong-capture/readme.md
---

# ci: run the Windows WARP legs on the NuGet WARP rather than the system one

CI's Windows jobs run sv's WARP legs on the WARP that ships with the runner image, and that WARP traces wrong.
[docs/bugs-external/warp-raytracing-wrong-capture](../bugs-external/warp-raytracing-wrong-capture/readme.md) has the evidence.
One test fails about one run in eight on it, and passes 80 runs on WARP 1.0.21 from NuGet.
Two sv tests are skipped on software adapters because of it (bug-sv-megakernel-emitter-capture-dark, and very likely bug-sv-warp-megakernel-quadric-flake).

Wanted: the Windows workflows put a pinned `Microsoft.Direct3D.WARP` `d3d10warp.dll` next to the test binaries before the test step, so those tests can run on WARP again.
A DLL beside the executable is found before the system one, which is all it took locally.

Decided already, and why this is CI-only:
- Not a configure-time fetch for every developer, unlike DXC.
  The package's terms are testing-only, forbid redistribution, and carry data collection and a US binding-arbitration clause.
  A build should not accept those on each person's behalf.
- So the download is a CI step, the way DLSS's SDK is a manual one, and a developer reproducing a CI failure does the same by hand.

Open:
- Whether the step belongs in the workflows, or in a `dev.py install warp` that CI calls; the latter keeps the pin in a `dependency.yml` with its hash.
- Whether to keep one leg on the system WARP, since that is what users without the package run.
