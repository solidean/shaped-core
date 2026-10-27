---
created: 2026-10-22
refs:
  - test: cnet - a refused connection fails with connection_refused
---

# A refused connection was once reported as an unknown error rather than connection_refused

Seen once in `dev.py check`'s relwithdebinfo leg on 2026-10-22, under the full parallel suite: `connect_op.failure` held no value, and the code read `unknown`.
The binary passed five times in a row on its own straight after.
Not looked into; the change it ran beside touched only shaped-rendering's tests.
