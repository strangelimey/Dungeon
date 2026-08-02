// ============================================================================
// Game/PartyHud.h — party-facing UI widgets (Game layer: they know Character).
//
// Umbrella header: includes the shared types and every party-HUD widget so
// existing call sites (GameUI, GameSettings) keep a single include. The
// implementations live one file pair per widget — see PartyBar,
// CharacterPanel, HandSlot, SpellbookPanel, InventoryWindow, CharacterSheet,
// plus PartyHudTypes / PartyHudDraw for the shared pieces.
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "Game/PartyBar.h"
#include "Game/CharacterPanel.h"
#include "Game/ControlBar.h"
#include "Game/HandSlot.h"
#include "Game/SpellbookPanel.h"
#include "Game/InventoryWindow.h"
#include "Game/CharacterSheet.h"
