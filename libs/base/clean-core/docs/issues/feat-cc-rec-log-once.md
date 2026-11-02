---
created: 2026-10-17
tags: [rec]
refs:
  - file: libs/base/clean-core/docs/logging.md
---

# Debounced and rate-limited logging

There is no `CC_LOG_*_ONCE` and no `log_once` helper, so every "warn once" site hand-rolls its own flag — sg alone has several, and the transfer work is about to add two more.

The shapes wanted are once-per-call-site and at-most-once-per-interval.
Both are keyed on the site rather than on a caller-supplied token, since a per-resource key is the caller's business.

Until it exists, a caller carries its own flag beside the state the warning is about.
