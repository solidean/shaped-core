# clean-core examples: one binary per file, each demonstrating one part of the API in practice.
# Run one with `uv run dev.py example clean-core/vector`; see docs/guides/examples.md.
sc_add_single_file_examples(clean-core
    FILES
    vector.cc
    string.cc
    result.cc
    map.cc
    system-info.cc
    LINK clean-core
    PCH CC_STD NEXUS
)
