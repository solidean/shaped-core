# api-surface

**The API this change moves, written in symbols.**

*Applies: when the change adds, removes or reshapes a public symbol — a header, an exported type, a CLI surface, a file format.
Skip it for a change that is entirely internal, and for a tool with no library API.*

## What it is for

API shape is the finding class that outlives every other.
A bug is fixed in an hour; a type that carves the problem at the wrong joint outlives several rewrites of its body — which is why
[reviewing-prs.md](../../../../docs/guides/reviewing-prs.md) ranks it equal-first with correctness.

Judging it needs the surface in front of you, all at once.
Spread across eight `changes` blocks it cannot be judged at all, because the question is about the *shape of the set*: what is
copyable, what owns what, which of these should not exist, and what a caller has to type.

So this entry is a cheat sheet of the surface after the change — near-real code, not prose about code.

## What goes in it

A `code` block holding the new and changed surface as it would appear in a cheat sheet:

```cpp
// new
struct sg::completion_group        // move-only, one fence per resource per direction
{
    completion_group(completion_group&&) noexcept;
    void wait() const;
};

// changed
- void ctx.signal(u64 value);
+ void ctx.signal(completion_group const& group);

// gone
sg::transfer_fence                  // replaced by the above
```

Then, in prose, only what the symbols do not say:

- **What is now expressible that was not**, and what is no longer expressible.
- **Ownership and lifetime**, where the type does not carry it.
- **Which library it landed in**, if that was a choice.
- **The call site**, three or four lines of what a caller actually writes.

## What to leave out

- **Anything internal** — `impl/` is not surface.
- **A symbol dump.**
  If it reads like generated output it is too long: this is a cheat sheet, and the judgement about what matters is the most useful thing in it.
- Prose restating a signature that is already shown.

## Shape

`show: collapsed` on any `changes` blocks: the symbols are the evidence here, and the diff is depth material.

The `ask` is usually about the shape rather than about a defect — *is this the right seam?* — with the alternatives as options.
Offer the alternative you would pick and say what changing it later would cost.

## Per repository

What counts as "public" is repository-specific, and for shaped-core it is:
a header in a library's exported `FILE_SET`, a symbol in a namespace other than `impl`, a `dev.py` subcommand, or a `.vdoc` format change.
Note that "no callers in the repo" is not evidence a symbol is dead — a consumer this tree does not contain may reach it.
