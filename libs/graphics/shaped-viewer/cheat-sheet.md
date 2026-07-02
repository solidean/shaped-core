# shaped-viewer cheat sheet

Professional, RTX-enabled visualization renderer. Namespace `sv`. Depends on shaped-rendering.
Headers are included by full path from `src/`: `#include <shaped-viewer/<name>.hh>`.

> **Scope note:** shaped-viewer is an early-stage skeleton — there is no public API yet. This
> sheet fills in as the renderer lands. Format conventions live in
> [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

```cpp
#include <shaped-viewer/all.hh>   // umbrella (currently just forward declarations)
```
