# qualified-type-definition — corpus

A header names its types in the library's `fwd.hh` and defines them qualified.
So a `class`, `struct` or `enum` **definition** inside an open namespace is the finding, and the namespace disappears from the definition site.

The blocks below are the boundary: what fires, how much of the namespace the fix moves, and what the rule deliberately never touches.
A block with no `path=` is linted as `<memory>`, which has no implementation extension and is therefore a header — the default this rule cares about.

## a struct in an open namespace

The plain case, and the one the fix was written for.

```cpp [qualified-type-definition]
namespace cc
{
struct span
{
};
}
```

## every record in the namespace is its own finding

The wrapper is removed once however many types it held, because the edits that remove it are byte-identical across the findings and the engine merges them.

```cpp [qualified-type-definition] [qualified-type-definition]
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

```cpp [qualified-type-definition]
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

```cpp [qualified-type-definition]
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

```cpp [qualified-type-definition]
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

```cpp [qualified-type-definition]
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

```cpp [qualified-type-definition]
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

```cpp [qualified-type-definition]
namespace cc
{
void f();
struct span
{
};
void g();
}
```

## a scoped enum is defined qualified like any other type

`enum class cc::direction : cc::u8 { … };` is valid C++ wherever the enum was declared first, which is what the library's `fwd.hh` is for.
The base gains a qualifier as well: it is looked up where it is written, and a bare `u8` only ever resolved through the namespace being left.

```cpp [qualified-type-definition]
namespace cc
{
enum class direction : u8
{
};
}
```

## the base takes the namespace's root, not its full name

`cc::console::u8` would find nothing — the using-directive re-exporting `cc::primitive_defines` sits in `cc`, and qualified lookup follows it from there.

```cpp [qualified-type-definition]
namespace cc::console
{
enum class color : u8
{
};
}
```

## a base that resolves on its own is left as written

A builtin and an already-qualified name both mean the same thing at file scope as they did inside the namespace.

```cpp [qualified-type-definition]
namespace cc
{
enum class direction : int
{
};
}
```

An already-qualified base is the other half of that.
It is `qualified-primitive`'s business — spelling it `cc::u8` inside `namespace cc` is what that rule hints about — and this rule leaves it untouched either way.

```cpp [qualified-type-definition] [qualified-primitive]
namespace cc
{
enum class direction : cc::u8
{
};
}
```

## an enum-base the linter cannot place is not rewritten on a guess

Which enclosing scope an unqualified name came from is exactly what a single-file linter cannot answer.
Only the re-exported integer aliases are known well enough to carry along, so any other bare base leaves the enum alone.

```cpp ~[qualified-type-definition]
namespace cc
{
enum class direction : my_alias
{
};
}
```

## a scoped enum needs no enum-base to be declared ahead of itself

Its underlying type is `int` unless it says otherwise, so `enum class cc::direction;` is a declaration the definition can refer back to.

```cpp [qualified-type-definition]
namespace cc
{
enum class direction
{
};
}
```

## an unscoped enum fires once it has fixed its underlying type

Without the `class`, the enum-base is what makes the enum declarable ahead of its definition.

```cpp [qualified-type-definition]
namespace cc
{
enum direction : u8
{
};
}
```

## an enum and a record beside each other are one run

Both carry a qualified name, so the whole block leaves the namespace together.

```cpp [qualified-type-definition] [qualified-type-definition]
namespace cc
{
enum class direction : u8
{
};
struct s
{
};
}
```

## an unscoped enum with no enum-base stays behind

Its underlying type is only known once the enumerators are, so the grammar gives it no opaque declaration — and a qualified definition has nothing to refer back to.
It keeps the namespace open around it, and ends the run there.

```cpp [qualified-type-definition]
namespace cc
{
enum direction
{
};
struct s
{
};
}
```

## a forward declaration stays behind

A qualified name may not *declare* a new entity, so `struct cc::fwd;` is not valid C++ and the declaration keeps its namespace.

```cpp [qualified-type-definition]
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

```cpp [qualified-type-definition] [qualified-type-definition]
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

```cpp [qualified-type-definition]
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

```cpp ~[qualified-type-definition]
namespace cc
{
struct s
{
} the_one;
}
```

## a record inside a function body is not the namespace's

The parser parents a function's declarations to the enclosing namespace, so the rule checks where the record actually sits.

```cpp ~[qualified-type-definition]
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

```cpp ~[qualified-type-definition]
namespace cc
{
struct span;
class string;
enum class seek_dir : u8;
}
```

## an anonymous enum has no name to qualify

```cpp ~[qualified-type-definition]
namespace cc
{
enum : u8
{
    first_flag = 1
};
}
```

## an enum definition that also declares a variable

Same as `struct S { } s;`: the variable would land at file scope if the type moved out, so neither half is reported.

```cpp ~[qualified-type-definition]
namespace cc
{
enum class direction : u8
{
} the_one;
}
```

## impl and custom are namespaces you are meant to see opened

Exempt at any depth, so `cc::impl` and a nesting inside `impl` are both silent.

```cpp ~[qualified-type-definition]
namespace cc::impl
{
struct helper
{
};
}
```

```cpp ~[qualified-type-definition]
namespace cc::custom
{
struct point
{
};
}
```

## an anonymous namespace has no name to qualify with

```cpp ~[qualified-type-definition]
namespace
{
struct local
{
};
}
```

## a translation unit defines what it likes

A definition in a `.cc` is that file's own business, and there is no include to leak the open namespace into.

```cpp ~[qualified-type-definition] path="a.cc"
namespace cc
{
struct span
{
};
}
```

## the qualified form the rule is asking for

```cpp ~[qualified-type-definition]
struct cc::span
{
};

enum class cc::seek_dir : u8
{
};
```
