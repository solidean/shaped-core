# blessed-includes

Every angle include that is not one of ours must be blessed by a `.shaped-lint.yml` above the file.
The default is deny, so the interesting cases are the ones that must stay quiet: our own headers, a header a macro spells, and any file no config reaches.

The `config=` on a block is the policy it is linted against, spelled in the same format the real files use.
A block without one is a file nothing was said about — the case every other rule's corpus relies on.

## an unblessed include is reported

```cpp [blessed-includes] config="rules:\n  - kind: allow-include\n    value: <type_traits>\n    reason: intrinsics\n"
#include <charconv>
```

## a blessed one is not

```cpp ~[blessed-includes] config="rules:\n  - kind: allow-include\n    value: <type_traits>\n    reason: intrinsics\n"
#include <type_traits>
```

## a denied include is reported, and its reason is the hint

The reason is the whole point of a `deny-include`: it names where to go instead.

```cpp [blessed-includes] config="rules:\n  - kind: deny-include\n    value: <mutex>\n    reason: use clean-core/thread/mutex.hh\n"
#include <mutex>
```

## one entry may bless a family

```cpp ~[blessed-includes] config="rules:\n  - kind: allow-include\n    value: <d3d12*.h>\n    reason: the dx12 gate owns them\n"
#include <d3d12.h>
#include <d3d12sdklayers.h>
```

## the SDK's two spellings are one header

Both `<dbghelp.h>` and `<DbgHelp.h>` are live in this tree, and the linker does not care which.

```cpp ~[blessed-includes] config="rules:\n  - kind: allow-include\n    value: <DbgHelp.h>\n    reason: the crash handler needs it\n"
#include <dbghelp.h>
#include <DbgHelp.h>
```

## our own headers need no blessing

A path says it is ours, and so does a bare `.hh` — our header extension, and no SDK's.
That second form is what keeps the generated shader headers out of every library's config.

```cpp ~[blessed-includes] config="rules:\n  - kind: deny-include\n    value: <mutex>\n    reason: use clean-core/thread/mutex.hh\n"
#include <clean-core/fwd.hh>
#include <sv_shaders.hh>
#include "sibling.hh"
```

## a header a macro spells is left alone

Which header `CONFIG_HEADER` names cannot be answered from this file, so nothing is guessed at.

```cpp ~[blessed-includes] config="rules:\n  - kind: deny-include\n    value: <mutex>\n    reason: use clean-core/thread/mutex.hh\n"
#include CONFIG_HEADER
```

## a file with no config above it is silent

This is what lets the configs adopt one library at a time, and what keeps every other corpus block quiet.

```cpp ~[blessed-includes]
#include <mutex>
#include <charconv>
```

## a file scope is drawn by the config, not by the rule

The rule knows nothing about `.hh` versus `.cc`; a `files:` glob is what draws that line, and here it lets a translation unit have what a header may not.

```cpp ~[blessed-includes] path="a.cc" config="rules:\n  - kind: allow-include\n    value: <memory>\n    reason: only in a TU\n    files: '**/*.cc'\n"
#include <memory>
```

```cpp [blessed-includes] path="a.hh" config="rules:\n  - kind: allow-include\n    value: <memory>\n    reason: only in a TU\n    files: '**/*.cc'\n"
#include <memory>
```

## a guarded include is still an include

Directives are opaque to the linter, so an include inside a `#if` reads as live code — the same corner the parser cuts.
Pinned as it behaves, not as a preprocessor would have it.

```cpp [blessed-includes] config="rules:\n  - kind: deny-include\n    value: <mutex>\n    reason: use clean-core/thread/mutex.hh\n"
#if 0
#include <mutex>
#endif
```

## each unblessed include is its own finding

```cpp [blessed-includes] [blessed-includes] config="rules:\n  - kind: allow-include\n    value: <type_traits>\n    reason: intrinsics\n"
#include <charconv>
#include <cstring>
```
