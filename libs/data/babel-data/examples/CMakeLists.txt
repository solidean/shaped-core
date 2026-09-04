# babel-data examples: one binary per file, each demonstrating one part of the API in practice.
# Run one with `uv run dev.py example babel-data/json-read`; see docs/guides/examples.md.
sc_add_single_file_examples(babel-data
    FILES
    json.cc
    LINK babel-data
    PCH CC_ALL NEXUS
)
