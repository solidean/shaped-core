# SGL Terminology

Kind of like a glossary.
We strive to be consistent with `sg`, not with any existing shader language.

* "inline constants". called push constants or root constants or SetBytes* in other apis
* "buffer"
* "texture"
* "binding" - we have binding groups and these hold descriptors
* shader types:
    * raster
        * "vertex"
        * "pixel" - not fragment
        * "geometry"
        * "tessellation eval/control" - TODO i feel like we want "tessellation" in the name but not sure about the other
    * mesh shading??
        * "mesh" - is this right? what about amplification? this is basically TODO
    * compute
        * "compute"
    * ray tracing - TODO: do we want a shared "ray" prefix or not?
        * "raygen"
        * "miss"
        * "callable"
        * "closest_hit"
        * "any_hit"
        * "intersection"
