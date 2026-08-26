# review — TODO

Ideas agreed but not built.
Each one is here because it was decided in a session and would otherwise be lost; none is a commitment to a design.

The known *gaps* — remote and mobile answering, the unimplemented `rank:` option kind, the missing offline form — live in the [readme](readme.md#not-yet) instead.
This file is for work that would add something the tool cannot do at all.

## Comments and questions anywhere

**The maintainer can only speak where the agent left an ask.**
An ask carries options plus a freeform box, and that is the whole channel back.
So a question about a `context/repo` block, or about one line of a hunk, has nowhere to go: it ends up in the text box of an unrelated ask, or back in chat.
Chat is the narrative the tool exists to replace, and an answer that silently belongs to a different block is worse than no answer.

Two anchors, both wanted:

- **Per block.** A small icon anchored top-right of every `## <type>` section, on hover.
  Every block type, not only `ask` — the context tiers are where the "why did we do it this way" questions actually land.
- **Per line of a change.** A comment on one line of a rendered diff, from the gutter.

### What the design has to respect

- **`entries/` belongs to the agent and `answers/` to the server.**
  A comment is maintainer-authored, so it is server-owned and must never be spliced into an entry file.
  A sibling `comments/<entry>.json`, or a section inside the existing answers file, both fit; the ownership split is the constraint, not the filename.
- **A line anchor must survive a re-ingest.**
  Line numbers move, so a comment cannot key on one.
  A change id plus the offset into that change's `.diff` body is stable exactly as long as the change id is.
  That is the right lifetime: when `sync` supersedes the change, the comment hangs on a hunk that no longer exists.
- **A comment discharges nothing.**
  Like the `tooling` group, it is not an answer to an ask, so it must not move the coverage gate.
- **It has to reach the agent through the round**, printed by `delta` alongside the answers, with enough context to be actionable on its own — which block, which change, which line.

### The open question

**A comment and a question are not the same thing**, and the tool probably needs to know which it got.
A comment is a remark the agent reads and may act on.
A question expects an answer back, which means the next round owes the maintainer a reply — and something has to track that a question is still unanswered, the way an ask is tracked today.

Whether that is one type with a flag, two types, or a comment that the agent turns into a real `ask` in the next round, is undecided.

## Superseding a block, not just an entry

**A partial round leaves earlier entries out of date, and there is no way to tighten them.**
Round 1 of pr-147 answered 020 and left 070 open — but 070's first defect only existed *because* 020 was unfixed, so answering 020 settled part of a question nobody had reached yet.
Today the only tools are appending a new block, which buries the correction at the bottom, or editing the old one in place, which rewrites history a maintainer already read.

`state:` can retire a whole entry.
Nothing can retire a block.

### The shape

**Rounds stay immutable, and the file stays append-only.**
A correction is a *new* block appended after the round boundary that names what it replaces, rather than an edit to the block above:

```markdown
## prose
supersedes: r1-defect-list

The two remaining defects, now that the parser is fixed.
```

The page renders the superseded block collapsed and struck, with the replacement in its place, so the maintainer can see both what it says now and what it said when they read it.
An agent reading the entry back gets only the live blocks unless it asks for the history.

That needs blocks to be **nameable**, which they are not today — only an `ask` has a name.
An optional `name:` on any block is the smaller half of this feature, and `supersedes:` is the larger.

### Marking what is new

A rephrase where three words changed is unreadable as a diff and dishonest as a silent replacement.
So a superseding block wants an inline **new** span — a markdown annotation the page highlights — letting a block be re-stated in full with only the changed points drawn.

This matters most exactly where partial rounds do.
The maintainer does not re-read an entry end to end before sending, so a correction buried in an unchanged paragraph is one they will not see.

### What has to be decided

- Whether `supersedes:` may cross entries, or only work within one.
  Within one is simpler and probably enough.
- Whether a superseded block still counts for `discharges:`.
  It must not, or a replaced ask would double-count.
- What `show --all` and `delta` print: live blocks only, or the chain.
- Whether the **new** span is a fenced role (`==new==`-style) or an attribute on the block.
  A span is what the rephrase case needs.

## An identity for a review

`init` gives a review a name and an optional `--title`, and the page's `<title>` is the literal string `review`.
Two small things:

- **Let the agent write the title.** It has read the range by the time `generate` runs, so a review can be called what it is about rather than `pr-147`.
- **A favicon.** Several reviews open in several tabs is the normal case, and they are currently indistinguishable.

Neither is load-bearing; both are the difference between a tool that feels finished and one that does not.

## Provenance

Raised while dogfooding the tool on its own PR (pr-147), 2026-08-26.
