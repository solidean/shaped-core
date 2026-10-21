---
created: 2026-08-26
refs:
  - file: tools/review/docs/review-folder.md
---

# Making a review durable: an export mode

A review folder is scratch under `.tmp/`, and the tool changes under it with no migration promise.
That is right while the tool is being built and wrong for a review anyone wants to keep.

**An export mode** would turn one into a durable artifact: the entries, the answers, the rounds and the attachments, in a form that does not depend on this version of the tool.
What that form is — a single markdown file, a static page, a zip — is undecided, and so is whether it can be read back.
