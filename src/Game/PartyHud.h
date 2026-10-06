// ============================================================================
// Game/PartyHud.h — party-facing UI widgets (Game layer: they know Character).
//
// Umbrella header: GameUI.h's one include for the shared types and the party
// widgets it holds. Each widget lives in its own file pair - PartyBar,
// CharacterPanel, ControlBar (the docks and the hand pairs), HandSlot,
// SpellbookPanel, PartyWindow, CharacterSheet - with PartyHudTypes and
// PartyHudDraw (included where drawn) for the shared pieces. The Minimal
// layout's cards (MemberCards) and the closed-panels tray (HudTray) are not
// here: only GameUI.cpp builds them, and it includes them itself.
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "Game/PartyBar.h"
#include "Game/CharacterPanel.h"
#include "Game/ControlBar.h"
#include "Game/HandSlot.h"
#include "Game/SpellbookPanel.h"
#include "Game/PartyWindow.h"
#include "Game/CharacterSheet.h"
