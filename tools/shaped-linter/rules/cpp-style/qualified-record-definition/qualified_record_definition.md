# qualified-record-definition — corpus

A header names its types in the library's `fwd.hh` and defines them qualified.
So a `class` or `struct` **definition** inside an open namespace is the finding, and the namespace disappears from the definition site.

The blocks below are the boundary: what fires, how much of the namespace the fix moves, and what the rule deliberately never touches.
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

The wrapper is removed once however many types it held, because the edits that remove it are byte-identical across the findings and the engine merges them.

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

## a class-template specialization is a definition too

The name is qualified like any other.
Its template arguments are looked up where it is written, so a rewritten specialization may need them qualified as well — the compiler is what says so, exactly as with a missing forward declaration.

```cpp [qualified-record-definition]
namespace tg
{
template <int D, class T>
struct object_traits<aabb<D, T>>
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

## a function in the namespace splits it rather than blocking the fix

The function cannot carry a qualified name, so it keeps the namespace; the record moves out above it.

```cpp [qualified-record-definition]
namespace cc
{
struct span
{
};
void f();
}
```

## a record between two functions is lifted out of the middle

What sits before it keeps its namespace block, what sits after gets a fresh one, and the order of the file never changes.

```cpp [qualified-record-definition]
namespace cc
{
void f();
struct span
{
};
void g();
}
```

## an enum stays behind

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

## a forward declaration stays behind

A qualified name may not *declare* a new entity, so `struct cc::fwd;` is not valid C++ and the declaration keeps its namespace.

```cpp [qualified-record-definition]
namespace cc
{
struct fwd;
struct s
{
};
}
```

## adjacent records move as one block

A run of definitions with nothing between them leaves the namespace together, so the file does not gain a namespace block per type.

```cpp [qualified-record-definition] [qualified-record-definition]
namespace cc
{
void f();
struct a
{
};
struct b
{
};
}
```

## an anonymous record stays where it is

It has no name to qualify, so it is never reported — and it ends the run around it, since a block that moves out cannot take it along.
The named record beside it still moves.

```cpp [qualified-record-definition]
namespace cc
{
struct
{
} anon;
struct s
{
};
}
```

## `struct S { } s;` stays too

The definition also declares a variable, and that variable would land at file scope if the type moved out.
Neither half is reported, and the run ends there.

```cpp ~[qualified-record-definition]
namespace cc
{
struct s
{
} the_one;
}
```

## a record inside a function body is not the namespace's

The parser parents a function's declarations to the enclosing namespace, so the rule checks where the record actually sits.

```cpp ~[qualified-record-definition]
namespace cc
{
constexpr auto make()
{
    struct seeder
    {
        int seed;
    };
    return seeder{};
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
