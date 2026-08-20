#pragma once

#include <clean-core/record/log.hh>

// CC_LOG_TRACE / DEBUG / INFO / WARNING / ERROR, so nobody has to know the folder is called record/.
//
// The machinery lives in clean-core/record/, because logging is one vocabulary over the event stream rather than a
// system of its own — see libs/base/clean-core/docs/systems/recording.md.
