# shaped-xr — VR and AR on sg, namespace `sxr`

sg rendering in a headset through OpenXR: the runtime, its session and frame loop, and the images it composes.
Depends on shaped-rendering, and on the OpenXR loader and an sg backend where the build has them.

Early stage — [docs/design.md](docs/design.md) is what exists, why, and what is planned.
[docs/testing.md](docs/testing.md) is how to run it without a headset, on Monado's simulated one.

```bash
uv run dev.py example xr-cube --window                          # vulkan, the room behind the cube where Link offers it
uv run dev.py example xr-cube --window --test-args=--dx12     # the same through the dx12 binding
uv run dev.py example xr-probe --window                         # what the active runtime offers for AR
```

```cpp
#include <shaped-xr/xr.hh>
auto system = sxr::system::try_create({.api = sxr::graphics_api::vulkan}).value();
auto ctx = system->create_context().value();          // on the runtime's GPU, to its spec
auto session = system->create_session(*ctx, {}).value();
while (session->poll_events())
    if (auto frame = session->wait_frame())
        session->submit_frame(render(frame.value())); // both eyes into frame.color's two layers
```
