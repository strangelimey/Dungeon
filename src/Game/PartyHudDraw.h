// ============================================================================
// Game/PartyHudDraw.h — shared draw helpers for the party HUD widgets.
//
// Free functions (not a class): CharacterPanel, HandSlot, InventoryWindow, and
// CharacterSheet all paint slots / portraits / resource bars the same way.
// ============================================================================
#pragma once

#include "Game/Character.h"
#include "Graphics/SpriteBatch.h"
#include "UI/Controls.h"

namespace dungeon::game {

// Shared background for item-bearing slots (hands, equipment doll, backpack):
// black, so the light-haloed 3D item icons read clearly against it.
inline constexpr Vec4 kSlotBg{0.0f, 0.0f, 0.0f, 1.0f};

// The FLAT bar: a theme-filled track, a coloured fill `fraction` wide, a 1px
// border. The skill XP bars use it, and so does the uiskin=0 debug look.
void DrawStatBar(gfx::SpriteBatch& batch, const gfx::Rect& rect, float fraction,
				 const Vec4& color, const ui::Theme& theme);

// --- the framed resource bars (docs/icon-updates-plan.md) --------------------
struct BarPulse;          // PartyHudTypes.h
struct ResourceBarStyle;  // PartyHudTypes.h

enum class ResourceBar { Health, Stamina, Mana, Food, Water };

// One resource bar: the procedural fill in the glass (SpriteBatch::DrawBarFill)
// and the iron frame around it. `tube` is the GLASS - the frame STICKS OUT past
// it on every side (FrameReach says how far), which is the layout's to make
// room for. `member` picks the heartbeat. Falls back to DrawStatBar over `tube`
// when the style is unframed or has no frame texture.
void DrawResourceBar(gfx::SpriteBatch& batch, const gfx::Rect& tube, ResourceBar which,
					 float fraction, size_t member, const ResourceBarStyle& style,
					 const ui::Theme& theme);
// The same in two passes, for a STACK of bars: every fill first, then every
// frame, so one bar's fill can never cover the scrollwork of the frame above
// it (and the fills batch into one draw, the frames into another).
void DrawResourceBarFill(gfx::SpriteBatch& batch, const gfx::Rect& tube, ResourceBar which,
						 float fraction, size_t member, const ResourceBarStyle& style,
						 const ui::Theme& theme);
void DrawResourceBarFrame(gfx::SpriteBatch& batch, const gfx::Rect& tube,
						  const ResourceBarStyle& style);

// How far the frame reaches past a tube of `tubeH` pixels, on each side.
struct BarFrameReach {
	float left, right, top, bottom;
};
BarFrameReach FrameReach(float tubeH);

// The heartbeat (Michael, 2026-09-30): resting, faster once the party is
// NOTICED (a monster in aggro), and much slower NEAR DEATH - which wins over
// noticed. No beat at all while down. Beats per minute.
float HeartRateTarget(const Character& member, bool noticed);
// Eases a pulse's rate toward `targetBpm` and advances its phase by `dt` real
// seconds.
void TickBarPulse(BarPulse& pulse, float targetBpm, float dt);

// The identity colour as it sits IN the stone: desaturated toward its own grey
// and darkened, so it marks a member without lighting up. The floor of every
// carved member groove (portrait frame, hand-pair frame); `down` dims it again.
Vec4 MutedIdentity(const Vec4& color, bool down = false);

// Baked portrait when present; otherwise the tinted square with the character's
// initial. The border is a groove carved round the portrait, its floor the
// muted identity colour - the same frame the member's hand pair wears.
void DrawIdentityBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect,
						const Character& character);
void DrawPortrait(gfx::SpriteBatch& batch, const gfx::Rect& rect,
				  const Character& character, const ui::Font& font,
				  const ui::Theme& theme);

struct ItemIconBank; // PartyHudTypes.h

// One rune face: the rune-item icon when loaded, else an element-tinted
// fallback square; element-coloured border. The spellbook's grid and sequence
// and a hand box armed with a spell all draw runes through this, so a rune
// reads the same everywhere. `disabled` washes it out under a dark overlay.
// `background` = false skips the face's own black fill, so whatever is under
// it shows through (a set hand's accent tint).
void DrawRuneFace(gfx::SpriteBatch& batch, const gfx::Rect& r, SpellSymbol s,
				  const ItemIconBank* icons, bool hot, bool disabled = false,
				  bool background = true);

// The colour a rune GLOWS in the Magic window: its school's - the very glyph
// colour its icon is drawn in (tools/BuildRuneGlow.py prints them) - and WHITE
// for the form runes, which belong to no school (Michael, ui-updates: "for now").
Vec4 RuneGlowColor(SpellSymbol s);

// A rune in the MAGIC WINDOW: its glyph alone, lit in RuneGlowColor over a soft
// halo of the same colour that PULSES slowly - `phase` in radians, so each rune
// can sit out of step with its neighbours. The caller draws the socket under it.
// Falls back to DrawRuneFace when the glow masks are not installed. `disabled`
// (spent / blocked) leaves the glyph dim and unlit.
void DrawRuneGlow(gfx::SpriteBatch& batch, const gfx::Rect& r, SpellSymbol s,
				  const ItemIconBank* icons, bool hot, bool disabled, float phase);

} // namespace dungeon::game
