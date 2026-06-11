#pragma once

#include "Core/Log.h"

#include <cstdlib>

// DN_ASSERT fires in all build types: this is a game, and silent corruption is
// worse than a crash with a message.
#define DN_ASSERT(cond, msg)                                                     \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ::dungeon::log::Error("Assertion failed: {} ({}:{}) — {}", #cond,    \
                                  __FILE__, __LINE__, msg);                      \
            std::abort();                                                        \
        }                                                                        \
    } while (0)
