# Linking

*Tracer: deliberately thin.*

A link gives the entries of one pipeline one numbering of what they share: the static samplers of its layout and the records of its callables.
Each entry is still checked and emitted from its own file, and only the numbers it reads change.
`sgl::link` is the call, and `sgl link` the command that wraps it.
Back to the [semantics](_index.md); the reasons are in [why/linking.md](why/linking.md).

## The group

* **LINK-1** A **link group** is the entries a host lists, each an entry point of a file of the library, with the options it is built with ([why](why/linking.md#link-1)).
  Each entry is built from its own file, as a compile of that file alone builds it, so a used module's entry points stay unbuilt with the program (CHK-350).
  One file with two sets of options is two files of the group.
  A group is one pipeline's stages: every entry a ray-tracing stage, or every entry a graphics stage, or one compute entry point alone.
  Any other mix is `link-mixed-stages`.
* **LINK-2** A sampler, a callables table and a ray set the group numbers are named by their module, `samplers.clamp`, wherever a file reaches them ([why](why/linking.md#link-2)).
  A symbol of a program file that names no module is named by the file's library path and its own name, `trace.sgl:gWater`.
  So a module's sampler is one sampler of the group, whichever entries reach it, and two programs' own samplers never meet.

## The numbering

* **LINK-3** The group numbers its entries by kind, and each kind in the order the host lists it ([why](why/linking.md#link-3)).
  A ray-tracing group's kinds are its raygens, its misses, its hit groups' stages, then its callables.
  A graphics group's are its stages in pipeline order: task, mesh, vertex, tessellation control, tessellation evaluation, geometry, pixel.
  The first kind is the **leading** one: a ray-tracing group's raygens, and a graphics group's first stage in pipeline order.
  A leading entry's file appends every file-scope sampler EMIT-133 numbers, in that order, and every table of its callable section (CHK-562), in that order.
  Every other entry appends what it reaches: the samplers its code reaches and the tables it calls, each in its file's order.
  A callable appends its own table too, so every callable the group lists has a record.
  Appending gives a sampler the next slot and a table the next record, unless the group numbers it already.
  Slots and records are dense, and two samplers of equal settings are two slots.
  A ray-tracing group without a raygen has no leading entry, and every entry appends what it reaches.
* **LINK-4** So the first leading entry's file numbers as it does compiled alone, and its entries are the outputs a compile of that file alone writes ([why](why/linking.md#link-4)).
  An entry whose samplers or tables take other numbers than its own file gives them is **renumbered**, and the group's map lists it.
* **LINK-5** A call `t[i](mut p)` calls the callable at `i` plus the **callable base** of `t`, a constant of the target ([why](why/linking.md#link-5)).
  The base is `t`'s first record in the group under a link, and `t`'s place in the program's callable section otherwise (CHK-562).
  A base of 0 adds nothing to `i`, so a compile with no link writes what CHK-344 has always written.
  A test runs with the program's section, as it reaches no call of a callable (CHK-344).
* **LINK-6** An entry emitted under a link reads each sampler at its group's slot, wherever EMIT-133 places a slot, and calls each table at its group's first record ([why](why/linking.md#link-6)).
  The group numbers from a check for no target, as `describe` checks, and each entry is emitted from its file's check for the target it is written for (CHK-426).
  A sampler or a table an emitted entry reaches and the group does not number is `unnumbered`.
  Its interface names each sampler by its LINK-2 name, as the map does, a sampler of the entry's own file too, while the text spells it as the file compiled alone does.
* **LINK-7** An entry's **numbering hash** is a hash of the slots and records it reaches, the same in every process ([why](why/linking.md#link-7)).
  What it reaches is what the check that numbers reaches and what each target the link checks for reaches, a branch on `target_api` or `has` included.
  So is what makes it renumbered (LINK-4).
  Two links that give one entry equal hashes emit it alike for one target and one set of options, so a cache of outputs keys on the hash beside the source.

## The map

* **LINK-8** The **link map** is what a host builds the group's pipeline from ([why](why/linking.md#link-8)).
  It holds the samplers by slot with their settings, the tables by first record with their callables, and the renumbered entries.
  It holds the group's ray set: every entry that traces uses it, and every miss and hit stage takes the payload of one of its rays.
  Where several sets agree with every entry, the one whose name sorts first by its bytes is the group's.
  Where none does, the link is `link-ray-set-mismatch`.
  A set's layout is CHK-329's: ray type r's contribution and miss index are r, and its multiplier is the set's ray count.
  A group whose entries trace, miss and hit nothing has no ray set.
  `has_host_callables` says the group's last table takes `.host`, so the host's callables follow its last record (CHK-343).

## Refusals

* **LINK-9** A link that cannot give a pipeline one numbering is refused, and its error names the group ([why](why/linking.md#link-9)).
  An empty group is `link-empty`.
  An entry whose file the library does not hold, which its file does not declare, or which is another stage than the one listed is `link-no-entry-point`.
  A sampler at a slot of 16 or more that an entry reaches is `too-many-samplers`, as it is for one file (EMIT-133).
  A table that takes `.host` with another table after it in the group is `link-host-callables`.
  An error of a file the group checks is that file's, as its compile alone reports it.

| kind | reported by |
|---|---|
| `link-empty` | LINK-9 |
| `link-no-entry-point` | LINK-9 |
| `link-mixed-stages` | LINK-1 |
| `link-ray-set-mismatch` | LINK-8 |
| `link-host-callables` | LINK-9 |
| `too-many-samplers` | LINK-9 |
| `unnumbered` | LINK-6 |

## Open

* `@raytracing pipeline` declarations as a second way to name a group, whose stages slib links the same way.
* Numbering what only a target's branch reaches, which today is `unnumbered` when no other entry numbers it; the map would then depend on the targets linked for.
* A check shared across links, which a bake of many groups over one library would read rather than check each file again.
* The polyfill forms of a graphics group's stages, which `link_result::emit` writes natively alone.
