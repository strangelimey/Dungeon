// ============================================================================
// Game/PartyHudDraw.h — shared draw helpers for the party HUD widgets.
//
// Free functions (not a class): CharacterPanel, HandSlot, the party window, and
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
// border. The uiskin=0 debug look, and the fallback of the framed bars below.
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

// The tube for a bar laid out in `box`, when its WHOLE frame - the glass plus
// the caps' reach above and below - must fit `roomH` (a row's pitch), or one
// bar's scrollwork runs into its neighbour's. The tube shrinks to fit, stays
// centred where the box was, and gives up the caps' reach across so the frame
// stays inside the box's width. Unframed styles get the box back unchanged.
// kFramedRowShare is how much of `roomH` the whole frame may take - a breath
// between one frame's foot and the next one's crown; a layout sizing a row
// for a given glass divides by it.
inline constexpr float kFramedRowShare = 0.94f;
gfx::Rect FitFramedTube(gfx::Rect box, float roomH, const ResourceBarStyle& style);

// A PROGRESS bar (the sheet's skills: the way to the next level, not a pool) -
// the Progress fill in `tint` inside the same frame, or the flat bar when the
// style is unframed. Fill and frame in one call: its rows are spaced by
// FitFramedTube, so no frame reaches a neighbour's fill.
void DrawProgressBar(gfx::SpriteBatch& batch, const gfx::Rect& tube, float fraction,
					 const Vec4& tint, float seed, const ResourceBarStyle& style,
					 const ui::Theme& theme);

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

// --- status effects ----------------------------------------------------------
// The share of an effect's time still to run, 0..1 - its icon's sliver. An
// effect with no duration (it lasts until something lifts it) reads full.
float EffectTimeLeft(const fx::Inst& effect);
// One effect's ICON in `rect`, the HUD strip's and the sheet's Effects tab's
// alike (code-review C263: two copies had drifted apart): an item socket
// (ui::DrawSlotFace), the kind's art in its well - a rune as its glyph alone,
// never the tablet - or a `tint` square without it, the time-left sliver
// (`frac`, EffectTimeLeft) draining along the picture's foot, and a border in
// `tint` (the effect's school). The picture's inset and the sliver's thickness
// are shares of the icon, so a small strip icon and a large tab icon draw alike.
void DrawEffectIcon(const ui::UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& rect,
					const fx::EffectKind* kind, const Vec4& tint, float frac,
					const ItemIconBank* icons);

// One rune face: the rune-item icon when loaded, else an element-tinted
// fallback square; element-coloured border. DrawRuneGlow's FALLBACK, for when
// the glow masks are not installed - the spellbook, the hand boxes and every
// other rune control draw through DrawRuneGlow, so a rune reads the same
// everywhere either way. `disabled` washes it out under a dark overlay.
// `background` = false skips the face's own black fill, so whatever is under
// it shows through (a set hand's accent tint).
void DrawRuneFace(gfx::SpriteBatch& batch, const gfx::Rect& r, SpellSymbol s,
				  const ItemIconBank* icons, bool hot, bool disabled = false,
				  bool background = true);

// The colour a rune GLOWS in the Magic window: its school's - the very glyph
// colour its icon is drawn in (tools/BuildRuneGlow.py prints them) - and WHITE
// for the form runes, which belong to no school (Michael, ui-updates: "for now").
Vec4 RuneGlowColor(SpellSymbol s);
// A rune TABLET's groove glow (MaterialParams::emissiveGroove): its mean, what
// the baked item icon holds, and how far the details dialog breathes it either
// side - on the same 3.4 s breath as the sockets' halo (kRuneBreathSeconds).
inline constexpr float kRuneGrooveMean = 0.35f;
inline constexpr float kRuneGrooveSwing = 0.18f;
inline constexpr float kRuneBreathSeconds = 3.4f;

// A RUNE'S HOVER TIP (ui-bars-updates P2): "Kenaz - Fire", its Futhark name and
// its meaning (rune.tip over rune.<id> + symbol.<id>), in the tooltip face the
// hand box's tip uses, placed by ui::PlaceTooltip off `anchor` (the rune's
// cell), below it by preference. Every place a rune glyph shows without words
// - Known Spells, the Magic window's grid and sequence, a set hand's recipe -
// draws through this, in its overlay pass. Formatted inline: no allocation.
void DrawRuneTip(ui::UIContext& ctx, gfx::SpriteBatch& batch, const ui::Font& font,
				 const gfx::Rect& anchor, SpellSymbol s);

// A rune in the MAGIC WINDOW: its glyph alone, lit in RuneGlowColor over a soft
// halo of the same colour that PULSES slowly - `phase` in radians, so each rune
// can sit out of step with its neighbours. The caller draws the socket under it.
// Falls back to DrawRuneFace when the glow masks are not installed. `disabled`
// (spent / blocked) leaves the glyph dim and unlit.
void DrawRuneGlow(gfx::SpriteBatch& batch, const gfx::Rect& r, SpellSymbol s,
				  const ItemIconBank* icons, bool hot, bool disabled, float phase);

// An ITEM in a socket `r` (the caller draws the socket): its icon, inset by
// `pad` of the socket's width. A RUNE is its baked CARVED TABLET with the
// school's halo breathing over the groove (Michael, 2026-10-02) - unless
// `symbolic`, which the HAND BOXES and the SPELL controls pass: there it is the
// glyph alone, lit as the Magic window draws it (DrawRuneGlow). Every item
// socket draws through this - hands, the doll, the backpack, the party
// inventory, the cursor. A BURNING item
// (a lit torch) wears its flame over the icon (DrawHeldFlame). False = nothing
// to draw (empty id, no icon).
bool DrawItemIcon(gfx::SpriteBatch& batch, const gfx::Rect& r, std::string_view typeId,
				  const ItemIconBank* icons, float pad = 0.1f, bool symbolic = false);

// A flame standing on the head of a burning item's icon `in`: `at` is the head
// in the icon (uv), the flame a stack of three tinted `flame` sprites (body,
// heart, core) that squash and sway out of step over a warm `glow`. It rises
// straight up whatever the torch's tilt in the icon, as a flame does. Animated
// off SpriteBatch::Time; allocation-free. `tint` (null = the ordinary orange)
// is a magical torch's flame colour.
void DrawHeldFlame(gfx::SpriteBatch& batch, const gfx::Rect& in, const Vec2& at,
				   const gfx::Texture& flame, const gfx::Texture* glow,
				   const Vec3* tint = nullptr);
// The flame itself, sized by the caller: standing on `head` (pixels), about
// `tall` pixels high, over a glow `glowSize` across. `seed` sets it out of step
// with any other flame. DrawHeldFlame sizes it to a socket; the item details
// dialog to the torch it turns.
void DrawFlame(gfx::SpriteBatch& batch, const Vec2& head, float tall, float glowSize,
			   float seed, const gfx::Texture& flame, const gfx::Texture* glow,
			   const Vec3* tint = nullptr);

} // namespace dungeon::game
