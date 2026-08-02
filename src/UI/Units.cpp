// ============================================================================
// UI/Units.cpp — see Units.h.
// ============================================================================
#include "UI/Units.h"

#include "UI/UIContext.h"

namespace dungeon::ui {

float Rem(const UIContext& ctx, float n) { return ctx.GetFont().Height() * n; }

float Em(const UIContext& ctx, float n) { return Rem(ctx, n); }

} // namespace dungeon::ui
