---
created: 2026-11-11
priority: next
refs:
  - test: "sv - a traced mean is the same bits*"
---

# The deterministic-seed test's hybrid mode differs in 29 channels on an RTX 4090

'sv - a traced mean is the same bits whatever ids and restarts its run went through' (from 11367e1af, merged on 2026-11-11) fails on the first iteration here: hybrid: 29 channels differ.
It blocked a ship of the cluster work, which was pushed with --no-test for want of time.
Not yet known whether the cluster_id field (ab2487fb6) or the test itself is the cause: a run with the prelude change reverted was interrupted.
Next step: rerun the test on b53adcdf7 with prelude/raytracing.sgl at fef36ca03, and on the parent that has no cluster commits.
