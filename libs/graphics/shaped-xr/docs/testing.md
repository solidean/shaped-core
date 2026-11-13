# Testing shaped-xr without a headset

`shaped-xr-test` holds two kinds of test.
The conversions and mappings need no runtime and always run.
`tests/session-test.cc` drives real sessions, and skips unless Monado's simulated headset is selected, because on any other runtime a session would start on someone's head.

## What Monado covers

It proves instance and system creation, the vulkan and dx12 bindings, a session reaching ready, the frame loop, and view instancing into the runtime's images.
Its simulated controllers prove the actions are accepted and their poses located, but nothing presses them, so an input a person gives stays unproven.
A drawn hand is checked by placing it in front of an eye and reading that eye back.
It proves nothing about Meta's AR extensions, which it does not have, and it composes nothing: its null compositor renders no image, so a test only reads back what it drew itself.
A session opts into that with `session_description::readable_color`, since on vulkan the frame is the runtime's own image.
The null compositor paces at 20 Hz, so each frame a test waits for costs 50 ms.
The OpenXR loader holds one instance per process, so these tests take `exclusive("openxr")` and run one at a time.

## Running it

```bash
uv run dev.py install monado     # builds the commit tools/monado.version pins into ~/tools/monado, Windows only so far
uv run dev.py test shaped-xr-test
```

**Every `dev.py test` then selects Monado by itself**, and no environment variable is yours to set.
It stops at tests on purpose: `dev.py example xr-cube` still goes to the headset, since the runtime an application gets is decided by `XR_RUNTIME_JSON`.
A runtime you chose yourself through `XR_RUNTIME_JSON` wins, and the session tests then skip.

To run an example on the simulated headset, select it by hand:

```bash
export XR_RUNTIME_JSON="$HOME/tools/monado/<commit>/build/openxr_monado-dev.json"
export XRT_COMPOSITOR_NULL=1
export SIMULATED_ENABLE=1
export SIMULATED_LEFT=simple SIMULATED_RIGHT=simple
uv run dev.py example xr-cube --window
```
