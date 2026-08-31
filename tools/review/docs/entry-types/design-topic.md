# design-topic

**One decision that will be hard to undo, put in front of the maintainer before the code exists.**

*Applies: a `design` review, which is every review with no changeset.*

A design review has no ledger, no coverage gate and no hunks, so none of the changeset-shaped types apply.
What it has instead is a set of choices, and the entry's whole job is to make one of them answerable in a click.

## The groups a design review has

`framing`, `topics`, `open-questions`, `tooling`, `finalize` — not the changeset groups.
`framing` is context-exempt like `meta`; everything in `topics` and `open-questions` owes all three tiers.

- **`framing`**, at `010`: what problem this is, and what would count as solved.
  It stands in for both `orientation` and `verdict`, which are changeset types.
  One entry, and it is where the reader learns why any of the rest matters.
- **`topics`**: one entry per decision, which is what this document is about.
- **`open-questions`**: the same shape, for a question whose answer does not block the design.
  The difference is what an unanswered one costs: a topic left open blocks the plan, an open question does not.
- **`tooling`**: friction in the review tool, as in any review.

## One decision per entry

The test is whether a maintainer could answer it without deciding anything else first.
"How is CPU topology modelled" is a topic.
"How should the API look" is four topics wearing one title.

An entry that ends with two asks about different subjects is two entries.
An entry with two asks about the *same* subject — the shape, and then a detail that only matters given that shape — is
one, and that is the common case.

## Write the option the code would actually take

A design entry's options are the thing being decided, so they have to be executable rather than directional.
"Use a stable id" is a direction; "enumerate descriptors with a stable string id, samplers key on the id" is a decision
someone can implement.

**Mark one `(recommended)` and say why in a `recommendation` block**, separately from the `prose` block that describes
the situation neutrally.
A maintainer who disagrees with the recommendation should still be able to trust the description.

**Offer the option you do not want**, spelled fairly.
An entry whose alternatives are obviously bad is not asking a question, and the answer it gets back means nothing.

## Show the shape in code

A design review has no hunks, so a `code` block is the only way the reader sees what is actually proposed.
Put the struct or the signature in, not a paragraph about it — the same rule the changeset reviews follow, and more
load-bearing here because nothing else in the entry is code.

Two versions side by side is often the clearest form: the shape that is obviously wrong, then the one being proposed,
so the reader sees what the decision buys.

## Name what it costs to get wrong

This is what separates a design entry from a survey.
Every topic should say, in a sentence, what a caller is stuck with if the wrong option is taken — a rename across every
call site, a silent wrong number, a break in a file format.

Where the answer is genuinely cheap to change later, say that too, and expect a fast answer.

## What does not belong

- **An implementation plan.** The review settles what to build; how to sequence it is the plan's job.
- **Anything the answer cannot change.** A topic whose options all lead to the same code is a note, not a question.
- **A summary of the other entries.** The tiers already carry what a reader needs; a topic that restates the framing
  teaches the reader that the collapsed sections are noise.
