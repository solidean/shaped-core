# babel-serializer examples: one binary per file, each demonstrating one part of the API in practice.
# Run one with `uv run dev.py example babel-serializer/chrome-trace`; see docs/guides/examples.md.
sc_add_single_file_examples(babel-serializer
    FILES
    chrome-trace.cc
    json.cc
    LINK babel-serializer
    PCH CC_ALL NEXUS
)
