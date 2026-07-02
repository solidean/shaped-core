# shaped-rendering cheat sheet

Render routines and helpers on top of shaped-graphics. Namespace `sr`. Depends on
shaped-graphics. Headers are included by full path from `src/`:
`#include <shaped-rendering/<name>.hh>`.

> **Scope note:** shaped-rendering is an early-stage skeleton — there is no public API yet.
> This sheet fills in as routines land. Format conventions live in
> [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

```cpp
#include <shaped-rendering/all.hh>   // umbrella (currently just forward declarations)
```
