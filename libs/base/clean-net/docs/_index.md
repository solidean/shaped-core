# clean-net docs

The `cnet` networking library.
Start at the [readme](../readme.md) for what it is, and the [cheat-sheet](../cheat-sheet.md) for the API at a glance.

- [structure.md](structure.md) — the layering, the per-platform support matrix, what is done and what is planned.

Repo-wide context worth having beside these:

- [docs/platforms.md](../../../../docs/platforms.md) — the support tiers, and why wasm being tier 1 shapes this library.
- [logging](../../clean-core/docs/logging.md) — every diagnostic here goes through `cc::rec`, on the `cnet` domain.
