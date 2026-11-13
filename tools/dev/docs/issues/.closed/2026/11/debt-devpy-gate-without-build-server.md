---
created: 2026-11-07
tags: [dev.py]
refs:
  - file: tools/dev/lib/pipeline/build_server.py
  - symbol: GATE_USES_BUILD_SERVER
  - file: tools/dev/cmd/check.py
closed: 2026-11-12
resolution: fixed
---

# check and ship build without the optional build server, even when it is opted in

`dev.py build` and `dev.py test` build through the optional local build server when a project or developer opts in (lib/pipeline/build_server.py).
`check`, and so `ship`, deliberately do not yet: the gate must stay independent of a new component until that component has proven itself on real work.
Every gate build already asks `Context.build_server(gate=True)`, which answers None while `GATE_USES_BUILD_SERVER` is False.

Turning it on is that one constant, once the server has run the daily `build` and `test` loop without a wrong result for a while.
What would justify it: a record of builds through the server with no result that differed from a plain build, and its fallback seen working in the gate's own setting.

## Resolution

The server ran the daily `build` and `test` loop for several days with no wrong result, so the gate now builds and tests through it under the same opt-in.
`check --no-server` and `ship --no-server` keep the gate off it; `ship`'s proof stays a separate opt-in.
