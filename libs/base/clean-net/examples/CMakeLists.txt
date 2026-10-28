# clean-net examples: one binary per file, each demonstrating one part of the API in practice.
# Run one with `uv run dev.py example clean-net/download`; see docs/guides/examples.md.
#
# Every example plays BOTH ends, over loopback or in-process, so none needs the internet.
# An example that fails because a remote host moved teaches nobody anything.
sc_add_single_file_examples(clean-net
    FILES
    download.cc
    dev-server.cc
    datagrams.cc
    LINK clean-net
    PCH CC_STD NEXUS
)
