---
created: 2026-10-17
refs:
  - file: libs/data/versioned-document/src/versioned-document/impl/document_arena.hh
  - symbol: vdoc::impl::document_arena
  - symbol: cc::impl::intern_shard
---

# vdoc carries its own bump allocator, which belongs in clean-core

`vdoc::impl::document_arena` is a bump allocator behind a `cc::memory_resource`, so a whole document's storage frees in one release.
It belongs in clean-core as a general bump resource — a `cc::arena_memory_resource` next to `cc::system_memory_resource` — and this copy goes when that lands.
`cc::impl::intern_shard` already hand-rolls the same thing privately, which is the second copy that makes it worth having once.

[decisions.md](../decisions.md#the-document-arena-is-vdoc-local-until-clean-core-grows-one) records why it stays vdoc-local until then.
