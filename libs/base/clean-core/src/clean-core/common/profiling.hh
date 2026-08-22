#pragma once

#include <clean-core/record/scope.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/record/value.hh>

// CC_RECORD_SCOPE, CC_RECORD, CC_RECORD_MARK, CC_RECORD_STAT and CC_RECORD_ACCUM in one include.
//
// The machinery lives in clean-core/record/, because profiling is one vocabulary over the event stream rather than a
// system of its own — see libs/base/clean-core/docs/systems/recording.md.
