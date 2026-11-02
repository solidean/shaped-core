---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/binding/bindless_array.hh
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/binding/impl/bindless_array_state.hh
---

# A transient bindless element is freed early only on request

`arr.transient.release(view)` frees an element its owner is done with.
One nobody releases keeps its view, and the resource behind it, until the array fills.
If owners turn out to forget, there are two automatic steps, and neither is free.

**Freeing by age:** once per epoch, free every slot nobody acquired for N epochs.
It costs a sweep or per-slot bookkeeping each epoch, N is a policy someone has to pick, and an element wanted every N+1 epochs is re-minted each time.

**Weak-keyed slots:** a slot holds its view weakly, and is freed when the resource behind it dies.
That needs no policy, but it needs a death notice from the resource to every array holding it.
And the descriptor still references the resource until the slot is cleared.
