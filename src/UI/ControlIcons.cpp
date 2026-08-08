// ============================================================================
// UI/ControlIcons.cpp — the shared control-glyph registry (see ControlIcons.h).
// ============================================================================
#include "UI/ControlIcons.h"

namespace dungeon::ui {
namespace {
ControlIcons g_icons;
} // namespace

void SetControlIcons(const ControlIcons& icons) { g_icons = icons; }
const ControlIcons& GetControlIcons() { return g_icons; }

} // namespace dungeon::ui
