// ============================================================================
// Core/Assert.h — fatal assertion macro.
//
// DN_ASSERT fires in ALL build types (including release): this is a game, and
// silent corruption is worse than a crash with a message. The message
// argument is only evaluated on failure, so it may be an expensive
// std::string expression (e.g. an std::expected's .error()).
//
// The failure goes through crash::ReportFatal rather than straight to the log,
// so the health record and a minidump both land BEFORE abort() — abort in a
// debug build raises a CRT dialog and leaves a process that looks alive, and
// anything deferred until after it never happens at all.
// ============================================================================
#pragma once

#include "Core/CrashHandler.h"
#include "Core/Log.h"

#include <cstdlib>
#include <format>

#define DN_ASSERT(cond, msg)                                                     \
	do {                                                                         \
		if (!(cond)) {                                                           \
			::dungeon::crash::ReportFatal(                                        \
				std::format("Assertion failed: {} ({}:{}) — {}", #cond,          \
							__FILE__, __LINE__, msg));                            \
			std::abort();                                                        \
		}                                                                        \
	} while (0)
