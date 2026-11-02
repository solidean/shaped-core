# no-stock-words

Prose avoids words that overuse has burned: they read as filler now, whatever they meant once.

The list sits in `no_stock_words.cc`, and each entry is a decision of its own, made with a measured count of its uses.
A word whose plain sense is common in code never belongs on it: `exactly` is the honest word for exact arithmetic.
The rule carries no fix, because the replacement says what the word only gestured at.

The block annotations are specified in [docs/coding-guidelines.md](../../../docs/coding-guidelines.md#the-corpus-format).

## The words it fires on

The hyphenated spelling, in a comment.

```cpp [no-stock-words]
// The second check is load-bearing: without it a stale handle is read.
```

The open spelling, in markdown.

````md [no-stock-words] path="x.md"
That ordering is load bearing for the cache.
````

Wrapped across two lines of one block.

```cpp [no-stock-words]
// The order of these two calls is load-
// bearing, so keep it.
```

## The shapes it stays quiet on

Code and inline code are never prose.

```cpp ~[no-stock-words]
auto const load_bearing = true; // see `load-bearing` in the spec
```

A word that merely contains the entry.

````md ~[no-stock-words] path="x.md"
The bearings take the load.
````
