# Why: linking

*Tracer: deliberately thin.*

The reasons behind the rules of [linking.md](../linking.md).
Nothing here is normative.

A program numbers its file-scope samplers (EMIT-133) and its callable section (CHK-562) by what it reaches.
A pipeline whose stages come from several files then holds stages that disagree on one static sampler table and one shader table.
Quest3D's hybrid trace met it: its light pick, built from its own module file, read `linearSamplerClamp` at slot 1 where the raygen's table holds it at slot 3.
A link moves the numbering from the program to the pipeline, and leaves the rest of checking and emitting as it was.

## LINK-1

The host knows which entries form a pipeline, and SGL does not: Quest3D's ray-tracing pipelines are fixed in its code, and slib's are declared in one file.
Building each entry from its own file keeps CHK-350, so a module stays a library and its entry points are what that file compiles.
A pipeline is the unit that shares one layout, so a group of stages of two pipelines would number for neither.
A compute pipeline has one stage, which its own file numbers already.

## LINK-2

A module's sampler is one object wherever it is reached, so the group's table holds it once.
A program file's own declarations are nobody else's, and its library path is the name no other program shares.

## LINK-3

A raygen keeps its file's whole table and section, so its output is the one compiled alone.
A host's table generated from that file's description then stays the pipeline's, as it was before any link.
The other stages append only what they reach, so a hit group's file does not bring every sampler its modules declare into the group's sixteen.
A later raygen of another file is allowed and appends, since a pipeline of two raygen files exists, and a hole in the table would waste a slot a stage cannot spare.
Merging equal settings would save slots, and it would make a sampler's slot depend on what other files declare; it waits for a pipeline that nears the limit.
A graphics pipeline follows the same rule, its first stage leading, since nothing in it is particular to ray tracing.

## LINK-4

An entry of the leading file is written as it is compiled alone, so a host that compiles it either way gets one output.
The renumbered entries are the ones whose outputs exist only under the link, which a host keys and caches apart.

## LINK-5

The check pass flattens each file once, before any group is known, so a flattened call cannot hold the group's number.
It keeps the table instead, and the number is a constant resolved when the entry is written for a target.
It is resolved before the tree is legalized, so every fold and every pass after it sees the constant as it saw the literal before.
A base of 0 leaving `i` alone keeps every output of an unlinked compile byte for byte what it was.

## LINK-6

A target's register of a sampler and its record of a callable are the only places the numbering reaches, so changing those is the whole of a link's effect on an output.
The numbering is every target's, so the host builds one table per pipeline whichever backend runs it, and a check for no target is what `describe` reports to it.
The interface names a sampler as the map does because a host fits a stage to its layout by name.
A file's own sampler would otherwise be bare in that file's stages and qualified in another file's that uses its module: two names for one slot of one table.
The text keeps its spelling, so an entry whose numbers are its file's is still the binary it is compiled alone (LINK-4).

## LINK-7

An entry's output depends on the numbers it reaches and on nothing else of the group.
So one entry in two pipelines that agree on its numbers is one output, and a bake compiles it once.

## LINK-8

A trace's contribution, multiplier and miss index are positions in its ray set (CHK-329), so a pipeline's shader table is laid out for one set.
A miss or a hit stage names no set, only a payload, so the set is the one its payload belongs to.

## LINK-9

A stage holds sixteen static samplers on Metal and WebGPU, so a slot past that is refused as one file's would be, with the group named, since the group made the slot.
The host's callables follow the section's last record, so a `.host` table anywhere else would hand an index meant for the host to another table.
