# clean-simd docs

[readme.md](../readme.md) is the orientation and [cheat-sheet.md](../cheat-sheet.md) the API at a glance.

- [design.md](design.md) — the decisions behind the types, the masks, the operator rule and the generator, each with what would reopen it.
- [op-matrix.md](op-matrix.md) — generated: which operations are operators per element, and what each kernel emulates.
- [TODO.md](TODO.md) — known follow-ups.

## Repo-wide context

- [docs/platforms.md](../../../../docs/platforms.md#x86-64-feature-level-sc_x64_level) — `SC_X64_LEVEL` and `SC_WASM_SIMD`, the floors every kernel sits on.
- [clean-core system info](../../clean-core/docs/systems/system-info.md) — `cc::get_cpu_features()`, the question runtime dispatch asks.
