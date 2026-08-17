# Concept: what lives outside the document

The document is not the application's state; it is the part of it worth keeping forever.

Outside it, and deliberately:

- **Selection, viewport camera, tool state, undo cursor** — local, per-user, and not history.
  A file stores this as *workspace* state, which creates no op and never makes a document look unsaved.
- **Projections** — render jobs, acceleration structures, outliner trees, inspectors, diff views.
  All derived, all rebuildable, none stored.
- **Permissions** — enforced by whatever serves shared history, by validating ops against path rules.
  Read-only users, annotation-only users and proposal workflows all fall out of path-based rules, and none of them needs a storage primitive.

[Snapshots](snapshots.md) belong to the same family and are worth naming separately, because they look like state and are not.
A snapshot is a cached materialization, derived from ops that are still there, and safe to drop at any moment.
The one exception is a snapshot a prune made load-bearing, which is the point at which it stops being derived — see [pruning and recovery](pruning-and-recovery.md).
