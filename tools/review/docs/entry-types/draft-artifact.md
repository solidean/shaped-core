# draft-artifact

**The thing the review is about to produce, shown as itself, before it is produced.**

*Applies: a `pr-comment` or `land-changes` review, once most entries are settled and before `finalize`.*

Numbered `985`, immediately before `990`, in group `finalize`.

## What it is for

Every other entry asks about the change.
This one asks about the review's own output: here is the comment I would post under your name, word for word — post it, or tell me what to fix.

The maintainer has approved each finding as it was raised, one at a time, over several rounds.
They have not seen what those findings look like assembled, in order, at the length someone else will read them.
Those are different judgements, and only the second one catches the comment that is technically accurate and three times too long.

**It is the last thing that cannot be checked any other way.**
An `finalize` artifact is a mechanical gather of what was decided.
Whether it opens well, whether the ordering makes the important finding the first one read, whether the tone fits someone else's branch — none of that is in the ledger.

## When to add it

When the answers stop producing follow-ups.
That is usually a round where every ask came back settled and nothing you wrote in response needs another round — not a fixed round number.

Adding it too early wastes a round on prose that will change.
Adding it after `finalize` inverts the point: the maintainer is then reviewing something already assembled rather than deciding what it should say.

## How to write it

**Put the real text in a `prose` block, as the comment.**
Not a summary of it, not bullet points about it.
If it would be posted as markdown with three headings and a table, write three headings and a table.
The maintainer is proofreading, and a paraphrase is not proofreadable.

**Say what you left out and why**, in a short paragraph *before* the draft.
An LGTM entry that produced no instruction, a finding the maintainer rejected, a nit not worth an author's afternoon.
The cuts are the editorial judgement, and they are what the maintainer is really being asked to check.

**Keep it in the review's own voice, addressed to the author.**
It is going on their branch, not into the review folder.

**Write it for someone holding the diff and nothing else.**
The author never sees the entries, the answers, or the rounds that produced them.
Anything the review *decided* has to arrive as an instruction they could carry out today — what to derive a value from, which symbol to add, which call site to change.
A conclusion that was obvious after three rounds of discussion reads as a hand-wave to the one person who was not in them.
The reviewer is the last one able to notice, being still full of the context that is about to be dropped.

**Check it with an agent that has only what the author will have.**
Give a subagent the branch and the comment, withhold everything else, and ask per item whether it could implement the change or would have to come back and ask.
Have it look up every symbol the comment names.
The two things it catches are a decision that lost its specifics on the way out, and a claim about the code that stopped being true.

**Group it `finalize`**, which exempts it from the three context tiers.
A draft comment needs no cold tier: it is not a finding, and the reader has been in the review for several rounds by the time it appears.

## The ask

One ask, and it stays simple.
The maintainer has already made every substantive decision, so this is a gate rather than a question.

```markdown
## ask  post-it

Post this after you send the round, or change it first?

- radio: post it as it stands  (recommended)
- radio: post it with the changes I noted below
- radio: do not post yet — another round first
```

A freeform box is always there, and it is where the line edits go.
Resist adding options for tone or length: an option list invites a choice between things you wrote, and what is wanted here is the maintainer's own words about their own comment.

## What not to do

Do not discharge changes from it.
It accounts for nothing in the ledger — every hunk was discharged by the entry that raised it, and pointing `discharges:` here would move coverage onto a formatting decision.

Do not post on the answer alone.
"Post it" in a round is approval of the text; the go-ahead to actually post is a separate, explicit instruction, and the review folder is not the place it arrives.
