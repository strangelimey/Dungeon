// ============================================================================
// Core/Assert.h — fatal assertion macro.
//
// DN_ASSERT fires in ALL build types (including release): this is a game, and
// silent corruption is worse than a crash with a message. The message
// argument is only evaluated on failure, so it may be an expensive
// std::string expression (e.g. an std::expected's .error()).
// ============================================================================
#pragma once

#include "Core/Log.h"

#include <cstdlib>
#define DN_ASSERT(cond, msg)                                                     \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ::dungeon::log::Error("Assertion failed: {} ({}:{}) — {}", #cond,    \
                                  __FILE__, __LINE__, msg);                      \
            std::abort();                                                        \
        }                                                                        \
    } while (0)
