#pragma once

#include <clean-core/string/string_view.hh>

namespace slib_test
{
/// tests/data/binding-corpus.txt, baked in at configure time.
/// Baked rather than read, so the test needs no path into the source tree and a corpus edit re-runs configure.
[[nodiscard]] cc::string_view binding_corpus_text();
} // namespace slib_test
