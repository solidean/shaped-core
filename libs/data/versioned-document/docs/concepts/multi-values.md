# Concept: multi-values

When two ops write the same path and **neither is an ancestor of the other**, there is no answer to which came later.
The storage layer refuses to invent one.

Both writes survive.
The property becomes multi-valued, and the raw document keeps every surviving `(writer op id, value)` pair.

```text
raw_property = [ (op_a, 10), (op_b, 20) ]
```

Nothing is lost, nothing is guessed, and a later op that writes the path resolves it back to a single value — because that op dominates both.

## The granularity is the whole property

A property is multi-valued or it is not.
Parts *within* a value never conflict independently: two concurrent writers of `transform/position` produce two whole positions, never a merged position with one writer's `x` and the other's `y`.

That coarseness is chosen, not conceded.
Sub-value merging would mean the storage layer understanding value structure, which is exactly the knowledge this design keeps out of it.
A half-and-half position is also a state neither author ever authored.

## Writers that agree still conflict structurally

If two concurrent writers write the *same* bytes, the property is **still** structurally multi-valued.
The storage layer records what happened, and what happened is two independent writes.

The [interpretation](interpretation.md) layer is where that stops mattering.
Parsing sees the writers agree — a memcmp, thanks to canonical [values](values.md) — and collapses the property silently to that value.
It records it in the report's *agreed multi-values* side channel, as a tidy-up hint for a later write.
No diagnostic, no user-visible conflict.

This case is common in practice, it is easy to get wrong, and it has its own tests.

## Genuine disagreement is the policy's problem

When the surviving writers disagree, the parse policy picks.
The default is deterministic and biased toward the local user:

- if exactly one surviving writer is inside the local closure — the set of ops reachable from the local head — that value wins, and a *remote conflict* diagnostic reports it;
- otherwise the smallest op id wins, and a *multi-valued conflict* diagnostic reports it.

Both branches are total and reproducible: the same inputs always produce the same document, on every machine.
