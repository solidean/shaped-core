# example-showcase

**The example itself, and the picture it produces, put in front of the maintainer.**

*Applies: whenever the change touches an example or a capture — the example's source, its sidecar, or a committed
reference image.
Skip it only where the touch is mechanical and could not change what the example shows: a rename, a formatting sweep,
a bulk include fix.*

## What it is for

An example is a thing someone will read, and a reference image is a thing someone will look at.
A diff shows neither.
The maintainer approving a hunk in `hello-cube.cc` is approving a demonstration nobody demonstrated, and a `Bin 0 ->
31695 bytes` line is a picture nobody saw.

So the entry shows them: the code as it reads after the change, and the image as it renders.
What comes out of *looking* is then a finding like any other, and it belongs in this entry rather than in one of its own —
the picture is the evidence, so the point wants to sit next to it.

This is where the highest-value findings on an example change actually are.
A capture that neither crashes nor asserts routinely shows nothing worth looking at, which is the whole argument
[docs/guides/examples.md](../../../../docs/guides/examples.md) makes for the authoring loop.
A review that reads the hunks and not the image inherits exactly that blind spot.

## Distinct from example-evidence

[example-evidence](example-evidence.md) is about **output the tool captured**, in an `example` block, for a text example.
This one is about **the source and the image**, and it is written by hand.
A change touching a graphical example usually wants this; one touching a text example usually wants that; a change
touching both wants both, and they are still two entries because they answer different questions.

## What goes in it

- **The example's body**, in a fenced block with its path.
  Small examples go in whole — under about forty lines of body, that is the honest thing to show.
  Larger ones go in as a summary plus the code that carries the point: the bring-up the change touched, the loop hooks,
  the call the example exists to demonstrate.
  Never a paraphrase of code you did not quote.
- **How it is captured**, in one or two lines: the sidecar's mechanism, its resolution, whether it names a capture.
- **Every image, inline.**
  Copy it into the review's `attachments/` and reference it as `<img src="/attachments/<name>.jpg">`; the server serves
  that folder and nothing else.
  A pair — committed against regenerated — goes side by side at `width:49%`.
- **What the picture actually shows**, written as observation before judgement.
  Name what reads and what does not: framing, lighting, whether the thing the example is *about* is visible at all.
- **What the picture shows that the code did not.**
  A clipped label, a panel at its auto-fit width, a glyph the font cannot draw, a scene pushed off-centre.
  These are invisible in a diff and obvious in a JPEG.

## How to look at an image

Open it.
Do not infer it from the code.

Then ask, in this order:

1. **Is the subject legible?** Is the thing the example demonstrates actually in frame and distinguishable.
2. **Does the picture show what the code claims?** An example declaring six face colours whose image shows one has a
   framing problem, not a rendering problem.
3. **Is any text in it right?** UI panels carry strings, and a committed image is where a missing glyph, a clipped
   label or a wrong plural becomes permanent.
4. **Is this a first run?** A capture that skips restored session state shows defaults nobody has looked at in months,
   and that is frequently where the defect is.

## What the ask looks like

The image is a decision about the example, so the options are about the example:

```markdown
## ask  cube-viewer-panel

The viewer's first-run panel is ~110px wide and its text wraps to two words a line.

- radio: add `SetNextWindowSize` to the viewer and fix both em-dashes, then refresh the images  (recommended)
- radio: fix the window size only
- radio: leave the examples; record it as a TODO
```

**Offer the deferral.** An example being imperfect is not a reason to hold a change, and a review that only ever says
*fix it* pushes toward a completeness nobody asked for.

## Where it sits

With the other `meta` entries, right after orientation and the glossary — `020`, `022`, `024`.
The group's own description is "what this change is, the API it moves, and the examples that show it", so this is its
home rather than a finding group.

One entry per example, or one per closely-related pair that shares its bring-up.
Two examples built on the same `app` class are one entry; two unrelated demonstrations are two.

This entry usually discharges nothing.
It is a showing rather than an accounting, and the hunks it quotes are discharged by whichever finding reads them.
