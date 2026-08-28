# example-evidence

**What the examples actually printed, run rather than described.**

*Applies: when the change adds examples, or reworks existing ones enough that a reader would want to see them run.
Skip it for a change that touches an example only incidentally — a rename, a formatting sweep.*

## What it is for

An example is judged by whether reading it teaches you the library, and a diff cannot show that.
A reviewer reading the hunks learns that the code changed; the maintainer is left deciding about a demonstration nobody demonstrated.

So the reviewer runs them, and the entry carries what came out.
The maintainer's decision is then the one they actually have — is this a good demonstration, and do I want to see the diff behind it — rather than a judgement about hunks.

## The block does the work

This is the one entry type with a block of its own, because the alternative is an instruction to an agent to quote output faithfully, and that is exactly the kind of claim nobody downstream can check.

```markdown
## example  clean-core/vector
source: libs/base/clean-core/examples/vector.cc:10-40
run: uv run dev.py example clean-core/vector
capture: stdout
```

`uv run review.py run <name>` executes it, writes the capture into `attachments/`, and splices back the output, the head sha and the time.
The grammar is in [block-grammar.md](../block-grammar.md#example-blocks); what matters when writing one of these entries is below.

## How to write one

**Run what the change touched**, not the corpus.
There is deliberately no `--all` in `dev.py example`: running everything would open every window it has.

**Cluster by what is demonstrated**, not one entry per binary.
Three examples that all show the same API from different angles are one entry with three blocks; two unrelated demonstrations are two entries.

**Never describe output you did not capture.**
A `run:` block is the tool's claim that it produced what is shown.
Where you ran something yourself — a tool the whitelist does not allow, the instruction tracer — use `cmd:` with the output written into `attachments/`.
The page then says it was not reproduced by the tool.

**Say what could not be captured**, rather than leaving it out.
An example that opens a window has no headless mode yet, so it carries `cmd:` and its command with no output, and `status: not-automatable` where that is the reason.
A reviewer who meets a blank there should see the tool's limit rather than wonder about an omission.

## What the ask looks like

Two decisions, and they are the maintainer's real ones:

```markdown
## ask  vector-example
Does this demonstrate what a reader needs?

- radio: yes, this is what I would want someone to read  (recommended)
- radio: show me the diff — the output does not settle it
- radio: the example demonstrates the wrong thing
- check: it should also show the failure case
```

## Screenshots are somebody else's block

**A graphical example has a headless capture mode now**, so a screenshot is no longer a manual drop.
`uv run dev.py example <match> --capture` writes an image with no display, and `--update-captures` sweeps every example a `.capture.json` declares.

That belongs in an [example-showcase](example-showcase.md) entry rather than here.
This type is about output the tool ran and captured; an image wants to be looked at rather than quoted.
