# qualified-record-definition — corpus

A header names its types in the library's `fwd.hh` and defines them qualified.
So a `class` or `struct` **definition** inside an open namespace is the finding, and the namespace disappears from the definition site.

The blocks below are the boundary: what fires, what only gets a hint, and what the rule deliberately never touches.
A block with no `path=` is linted as `<memory>`, which has no implementation extension and is therefore a header — the default this rule cares about.

## a struct in an open namespace

The plain case, and the one the fix was written for.

```cpp [qualified-record-definition]
namespace cc
{
struct span
{
};
}
```

## every record in the namespace is its own finding

The wrapper is removed once however many types it held, because the two edits that remove it are byte-identical across the findings and the engine merges them.

```cpp [qualified-record-definition] [qualified-record-definition]
namespace cc
{
struct a
{
};
class b
{
};
}
```

## a template is still a record definition

The template prefix stays where it is; only the name gains the qualifier.

```cpp [qualified-record-definition]
namespace cc
{
template <class T>
struct span
{
};
}
```

## a member record belongs to its record, not to the namespace

Only a direct child of the namespace fires, so a nested type is reported through its outer type and never separately.

```cpp [qualified-record-definition]
namespace cc
{
struct outer
{
    struct inner
    {
    };
};
}
```

## a nested namespace is unwrapped one level at a time

The inner namespace holds a record and fires; the outer holds a namespace, so it has no record of its own to report.

```cpp [qualified-record-definition]
namespace a
{
namespace b
{
struct x
{
};
}
}
```

## a forward declaration is not a definition

This is what leaves `fwd.hh` alone, and it is not an accident of the parser: the declaration is precisely what the rewrite everywhere else depends on.

```cpp ~[qualified-record-definition]
namespace cc
{
struct span;
class string;
}
```

## impl and custom are namespaces you are meant to see opened

Exempt at any depth, so `cc::impl` and a nesting inside `impl` are both silent.

```cpp ~[qualified-record-definition]
namespace cc::impl
{
struct helper
{
};
}
```

```cpp ~[qualified-record-definition]
namespace cc::custom
{
struct point
{
};
}
```

## an anonymous namespace has no name to qualify with

```cpp ~[qualified-record-definition]
namespace
{
struct local
{
};
}
```

## a translation unit defines what it likes

A definition in a `.cc` is that file's own business, and there is no include to leak the open namespace into.

```cpp ~[qualified-record-definition] path="a.cc"
namespace cc
{
struct span
{
};
}
```

## the qualified form the rule is asking for

```cpp ~[qualified-record-definition]
struct cc::span
{
};
```

## a function in the namespace costs the fix, not the finding

The record still has to move out — that is the mixed case.
But the function cannot come with it, so the rewrite is a hint rather than something `--fix` applies.

```cpp [qualified-record-definition]
namespace cc
{
struct span
{
};
void f();
}
```

## an enum in the namespace is the same shape

An enum is not a record and never fires on its own, and it cannot be defined qualified either, so it keeps the namespace open around it.

```cpp [qualified-record-definition]
namespace cc
{
enum class direction
{
};
struct s
{
};
}
```

## a forward declaration alongside a definition blocks the unwrap

A qualified name may not *declare* a new entity, so `struct cc::fwd;` is not valid C++ and the namespace has to stay.

```cpp [qualified-record-definition]
namespace cc
{
struct fwd;
struct s
{
};
}
```

## a trailing declarator on the record is not modelled

`struct s { } the_one;` declares a variable the record node does not carry, so unwrapping would drop it.
Pinned as it behaves: the finding stands, the fix does not.

```cpp [qualified-record-definition]
namespace cc
{
struct s
{
} the_one;
}
```
