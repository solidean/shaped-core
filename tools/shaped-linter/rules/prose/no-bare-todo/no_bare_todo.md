# no-bare-todo

A `TODO`, `FIXME`, `HACK` or `XXX` in a C++ or Python comment.
Open work is an issue, and the code names it with `issue: <id>`; [issue-tracking.md](../../../../../docs/guides/issue-tracking.md) is the system.

The block annotations are specified in [docs/coding-guidelines.md](../../../docs/coding-guidelines.md#the-corpus-format).

## The shapes it fires on

A bare marker, with or without a colon.

```cpp [no-bare-todo]
// TODO: handle the empty case
```

The other three words, one finding each.

```cpp [no-bare-todo] [no-bare-todo] [no-bare-todo]
// FIXME the HACK below, XXX
```

A doc comment and a block comment are comments too.

```cpp [no-bare-todo]
/// TODO: returns the size
```

```cpp [no-bare-todo]
/* XXX: not thread-safe */
```

A Python comment and a docstring.

```py [no-bare-todo] path="a.py"
# TODO: cache this
```

```py [no-bare-todo] path="a.py"
def f():
    """FIXME: the second call is wrong."""
```

## The shapes it stays quiet on

The marker that names its issue.

```cpp ~[no-bare-todo]
// issue: feat-example-open-work
```

A workaround naming its debt issue.

```cpp ~[no-bare-todo]
// TEMPORARY(debt-example-workaround): drop once the compiler is fixed
```

A mention in backticks is prose about the word.

```cpp ~[no-bare-todo]
// a `TODO` comment is an issue now
```

Lower case, and the word inside an identifier, are not the marker.

```cpp ~[no-bare-todo]
// the todo list, a Hackathon, and XXXL sizes
```

Code is not a comment, whatever it spells.

```cpp ~[no-bare-todo]
auto const s = "TODO";
int TODO_count = 0;
```

Markdown is not checked.

````md ~[no-bare-todo] path="a.md"
TODO is a word this document is allowed to use.
````
