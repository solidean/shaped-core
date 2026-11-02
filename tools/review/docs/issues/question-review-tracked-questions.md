---
created: 2026-08-26
refs:
  - file: tools/review/cmd/append.py
  - file: tools/review/cmd/status.py
---

# Should the tool track a question as something owed an answer?

Comments are remarks: the agent answers one by appending a block with `addresses:`, and nothing new is tracked.

**A question expects an answer back**, which the current shape does not model.
The agent replies in prose and the maintainer reads it in the entry, with nothing saying the reply is owed or has arrived.
That was deliberate: the tool has exactly one notion of something outstanding.
A second one means a second definition of "still open" and a second way for `status` to be wrong.

Revisit once comments have been used on a few reviews.
If the appended-ask path turns out to be enough, this closes; if maintainers keep asking questions that go unanswered, it is a real gap.
