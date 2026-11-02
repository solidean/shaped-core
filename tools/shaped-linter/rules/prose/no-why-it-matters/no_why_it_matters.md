# no-why-it-matters

Prose says what follows from a fact, rather than announcing that the fact matters.
An announcement points back at the sentence before it and asks the reader to take the weight on trust; the consequence itself is usually shorter.

The detector is a list of phrase patterns in `no_why_it_matters.cc`, matched over a whole prose block, so a phrase wrapped across lines is found too.
It carries no fix: the rewrite is the consequence, and only the writer knows it.

The block annotations are specified in [docs/coding-guidelines.md](../../../docs/coding-guidelines.md#the-corpus-format).

## The shapes it fires on

A heading announcing the section.

````md [no-why-it-matters] path="x.md"
## Why this matters
````

A bold lead-in does the same.

````md [no-why-it-matters] path="x.md"
**Why it matters:** a caller may hand any integer in an enum parameter.
````

A sentence opening on a pronoun that points back.

````md [no-why-it-matters] path="x.md"
This matters in mixed pipelines.
````

```cpp [no-why-it-matters]
// It matters because localhost resolves to both addresses.
```

A trailing clause.

```cpp [no-why-it-matters]
// The allocator is stateless, which matters here: a coroutine dies on another thread.
```

```cpp [no-why-it-matters]
// No allocation and no lifetime — which matters, because the slot is shared.
```

The intensified variant.

````md [no-why-it-matters] path="x.md"
Pseudo-interfaces are excluded, which matters more than it sounds.
````

````md [no-why-it-matters] path="x.md"
Taking the file's word matters more than it looks.
````

The promise of an explanation.

````md [no-why-it-matters] path="x.md"
Here's why: the browser cannot block at all.
````

````md [no-why-it-matters] path="x.md"
One page per release tells what it brings and why it matters.
````

The aside that points at the important half.

```cpp [no-why-it-matters]
// Each line names the module and — this is the part that matters — the byte offset.
```

A clause wrapped across two lines is still one clause.

```cpp [no-why-it-matters]
// Every fuzz test evaluates this before the body,
// which matters when one test runs tens of millions of them.
```

## The shapes it stays quiet on

A real subject names what matters, so nothing points back.

````md ~[no-why-it-matters] path="x.md"
The order matters.
Only the sign matters, never the magnitude.
````

A relative clause, even when it wraps to the start of a line.

```cpp ~[no-why-it-matters]
// the count that matters is offered against taken, and the only thing
// that matters for the gate is the second one
```

A condition or a place.

````md ~[no-why-it-matters] path="x.md"
Measure where it matters, and decide whether it matters before optimizing.
````

`which` as a question word.

````md ~[no-why-it-matters] path="x.md"
Decide which matters more.
````

`here is why` inside a sentence, as quoted speech.

````md ~[no-why-it-matters] path="x.md"
It says "I will not read this subtree, and here is why".
````

Inline code and code are never prose.

````md ~[no-why-it-matters] path="x.md"
a `This matters.` sample
````

```cpp ~[no-why-it-matters]
auto const s = "This matters.";
```
