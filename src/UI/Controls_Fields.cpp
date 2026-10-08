// ============================================================================
// UI/Controls_Fields.cpp - the controls that edit one value: Checkbox, Slider,
// ColorPicker, KeyBind, TextField.
// Split by widget family by code-review C127; declared in UI/Controls.h.
// ============================================================================
#include "UI/Controls.h"
#include "UI/Controls_Detail.h"
#include "Core/Utf8.h"

#include <algorithm>
#include <format>

namespace dungeon::ui {

// --- Checkbox ------------------------------------------------------------

void Checkbox::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) {
			m_checked = !m_checked;
			if (onChange) onChange(m_checked);
		}
	}
}

void Checkbox::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();
	const bool skinned = ctx.GetSkin() != nullptr;
	if (skinned) {
		// On stone the row states are light, not paint: the old brown fills
		// read as mud over the slab.
		if (highlight) {
			batch.DrawRect(px, {theme.accent.x, theme.accent.y, theme.accent.z, 0.12f});
			DrawBorder(batch, px, {theme.accent.x, theme.accent.y, theme.accent.z, 0.40f});
		} else if (m_hot) {
			batch.DrawRect(px, {1.0f, 1.0f, 1.0f, 0.06f});
		}
	} else if (highlight) {
		batch.DrawRect(px, theme.controlActive);
		DrawBorder(batch, px, theme.panelBorder);
	} else if (m_hot) {
		batch.DrawRect(px, theme.controlHot);
	}
	// The check box itself, left-aligned and vertically centered.
	const float box = BoxSide(px);
	const gfx::Rect b{px.x + Rem(0.15f), px.y + (px.h - box) * 0.5f, box, box};
	DrawFieldFace(ctx, batch, b, m_hot ? FieldState::Hot : FieldState::Normal, theme.control,
				  theme.panelBorder);
	if (m_checked) {
		const float in = box * (skinned ? 0.32f : 0.24f);
		const gfx::Rect tick{b.x + in, b.y + in, b.w - 2 * in, b.h - 2 * in};
		if (skinned) {
			// A lit mark in the well: a soft halo, then the tick.
			const float g = in * 0.3f;
			batch.DrawRect({tick.x - g, tick.y - g, tick.w + 2 * g, tick.h + 2 * g},
						   {theme.accent.x, theme.accent.y, theme.accent.z, 0.30f});
		}
		batch.DrawRect(tick, theme.accent);
	}
	// The swatch: square, the row's height less a hairline inset, after the box -
	// asked for now, at the moment it is drawn (see the header).
	if (swatch) {
		const float side = px.h - 2.0f;
		DrawSwatch(batch, {b.x + b.w + Rem(0.3f), px.y + 1.0f, side, side}, swatch());
	}
	font.Draw(batch, label, TextX(px), px.y + (px.h - font.Height()) * 0.5f,
			  (m_checked || highlight) ? theme.text : theme.textDim);
}

float Checkbox::TextX(const gfx::Rect& px) const {
	const float box = BoxSide(px);
	float x = px.x + Rem(0.15f) + box + Rem(0.3f);
	if (swatch) x += (px.h - 2.0f) + Rem(0.3f);
	return x;
}

gfx::Rect Checkbox::InkRect() const {
	// Mirrors DrawSelf's geometry: the box (and swatch) are bounded, the label
	// runs on past the bounds if it was not given the room. Vertically the label
	// is centred like a Button's, so a row shorter than the font spills the line
	// out of both edges.
	const gfx::Rect& px = Pixel();
	const float right =
		std::max(px.x + px.w, TextX(px) + TextFont().MeasureWidth(label));
	const float h = label.empty() ? px.h : std::max(px.h, TextFont().Height());
	return {px.x, px.y + (px.h - h) * 0.5f, right - px.x, h};
}

// --- Slider --------------------------------------------------------------

void Slider::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const bool hovered =
		!ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
	if (hovered && input->WasMousePressed(MouseButton::Left)) m_dragging = true;
	if (m_dragging && !input->IsMouseDown(MouseButton::Left)) {
		m_dragging = false;
		if (onRelease) onRelease();
	}
	if (hovered || m_dragging) ctx.ConsumeMouse();

	if (m_dragging) {
		const float t = std::clamp(
			(input->MouseX() - Pixel().x) / std::max(Pixel().w, 1.0f), 0.0f, 1.0f);
		const float value = m_min + t * (m_max - m_min);
		if (value != m_value) {
			m_value = value;
			RefreshDisplay();
			if (onChange) onChange(m_value);
		}
	}
}

void Slider::RefreshDisplay() {
	m_display = std::format("{}: {:.{}f}", label, m_value, m_decimals);
}

void Slider::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	// The whole control lives INSIDE its bounds: the label on the top line, the
	// track + thumb in the band beneath it. (Previously the label was drawn above
	// the bounds, so callers couldn't space sliders by their box — it collided
	// with whatever sat above.) Bounds should be tall enough for both.
	font.Draw(batch, m_display, px.x, px.y, theme.textDim);
	const float bandY = px.y + font.LineAdvance();
	const float bandH = std::max(px.h - font.LineAdvance(), Rem(0.3f));

	const float t = (m_value - m_min) / std::max(m_max - m_min, 1e-6f);
	const float thumbW = Rem(0.7f);
	const Skin* skin = ctx.GetSkin();
	if (skin && skin->button.texture) {
		// Skinned: the track is a sunken GROOVE (the field face, so it reads with
		// the drop-downs and text boxes) holding a lit accent strip, and the thumb
		// a raised stone button - the thing you grab stands proud of the thing it
		// slides in.
		const float grooveH = std::min(Rem(0.45f), bandH);
		const float grooveY = bandY + (bandH - grooveH) * 0.5f;
		const gfx::Rect groove{px.x, grooveY, px.w, grooveH};
		const gfx::Rect well = DrawFieldFace(ctx, batch, groove,
											 m_dragging ? FieldState::Active : FieldState::Normal,
											 theme.control, theme.panelBorder);
		batch.DrawRect({well.x, well.y, well.w * t, well.h},
					   {theme.accent.x, theme.accent.y, theme.accent.z, 0.85f});
		const float thumbH = std::max(bandH * 0.9f, grooveH + 4.0f);
		const float stoneW = Rem(0.9f);
		const gfx::Rect thumb{px.x + px.w * t - stoneW * 0.5f,
							  grooveY + grooveH * 0.5f - thumbH * 0.5f, stoneW, thumbH};
		DrawFace(batch, thumb, *skin, m_dragging ? Face::ButtonDown : Face::Button,
				 {1.0f, 1.0f, 1.0f, 1.0f});
		return;
	}

	// Track.
	const float trackH = Rem(0.15f);
	const float trackY = bandY + (bandH - trackH) * 0.5f;
	batch.DrawRect({px.x, trackY, px.w, trackH}, theme.control);

	// Filled portion + thumb.
	batch.DrawRect({px.x, trackY, px.w * t, trackH}, theme.accent);
	// A stubby block centred on the track: half the band tall, 0.7rem wide (it
	// was the full band and 0.35rem, and read as a thin post). Drawing only -
	// the whole bounds take the drag.
	const float thumbH = bandH * 0.5f;
	const gfx::Rect thumb{px.x + px.w * t - thumbW * 0.5f,
						  trackY + trackH * 0.5f - thumbH * 0.5f, thumbW, thumbH};
	batch.DrawRect(thumb, m_dragging ? theme.controlActive : theme.controlHot);
	DrawBorder(batch, thumb, theme.panelBorder);
}

// --- ColorPicker -------------------------------------------------------------

namespace {

// Popup geometry in REM (UI/Units.h). Four channel rows, each:
// letter | track | 0..255 value.
constexpr float kPickerPopupW = 11.4f;
constexpr float kPickerPopupPad = 0.45f;
constexpr float kPickerRowPitch = 1.3f;
constexpr float kPickerRowH = 1.0f;
constexpr float kPickerLetter = 0.95f; // column before the track
constexpr float kPickerValue = 2.0f;   // column after it
constexpr float kPickerPopupH =
	kPickerPopupPad * 2 + kPickerRowPitch * 3 + kPickerRowH;

float& Channel(Vec4& color, int index) {
	switch (index) {
	case 0: return color.x;
	case 1: return color.y;
	case 2: return color.z;
	default: return color.w;
	}
}

gfx::Rect PickerRow(const gfx::Rect& popup, int index, float rem) {
	return {popup.x + kPickerPopupPad,
			popup.y + (kPickerPopupPad + kPickerRowPitch * static_cast<float>(index)) * rem,
			popup.w - 2 * kPickerPopupPad * rem, kPickerRowH * rem};
}

gfx::Rect PickerTrack(const gfx::Rect& row, float rem) {
	const float letter = kPickerLetter * rem, value = kPickerValue * rem;
	return {row.x + letter, row.y, row.w - letter - value, row.h};
}

} // namespace

gfx::Rect ColorPicker::SwatchRect() const {
	const gfx::Rect& px = Pixel();
	const float w = std::min(Rem(2.3f), px.w * 0.45f);
	return {px.x + px.w - w, px.y, w, px.h};
}

gfx::Rect ColorPicker::PopupRect(const UIContext& ctx) const {
	const gfx::Rect swatch = SwatchRect();
	const float w = Rem(kPickerPopupW), h = Rem(kPickerPopupH);
	const float x =
		std::clamp(swatch.x + swatch.w - w, 0.0f, std::max(0.0f, ctx.Width() - w));
	float y = swatch.y + swatch.h + Rem(0.15f);
	if (y + h > ctx.Height()) // no room below: open above instead
		y = swatch.y - Rem(0.15f) - h;
	return {x, y, w, h};
}

void ColorPicker::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX();
	const float my = input->MouseY();

	if (m_open) {
		const gfx::Rect popup = PopupRect(ctx);
		if (m_dragChannel >= 0 && !input->IsMouseDown(MouseButton::Left))
			m_dragChannel = -1;
		if (m_dragChannel < 0 && input->WasMousePressed(MouseButton::Left)) {
			if (popup.Contains(mx, my)) {
				for (int i = 0; i < 4; ++i)
					if (PickerRow(popup, i, Rem()).Contains(mx, my)) m_dragChannel = i;
			} else {
				m_open = false; // click anywhere else (incl. the swatch) closes
				if (onClose) onClose();
			}
		}
		if (m_open && input->WasKeyPressed(vk::Escape)) {
			m_dragChannel = -1;
			m_open = false;
			if (onClose) onClose();
		}
		if (m_dragChannel >= 0) {
			const gfx::Rect track = PickerTrack(PickerRow(popup, m_dragChannel, Rem()), Rem());
			const float t =
				std::clamp((mx - track.x) / std::max(track.w, 1.0f), 0.0f, 1.0f);
			float& value = Channel(m_color, m_dragChannel);
			if (t != value) {
				value = t;
				if (onChange) onChange(m_color);
			}
		}
		// The open popup owns the mouse entirely — wheel included, so the page
		// behind cannot scroll the popup off its own swatch — and, through
		// ClaimPopup, before any control the walk reaches first.
		ctx.ConsumeMouse();
		ctx.ConsumeWheel();
		if (m_open) ctx.ClaimPopup();
		return;
	}

	m_hot = !ctx.IsMouseConsumed() && SwatchRect().Contains(mx, my);
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) OpenPopup(ctx);
	}
}

void ColorPicker::OpenPopup(UIContext& ctx) {
	m_open = true;
	ctx.ClaimPopup();
}

void ColorPicker::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	font.Draw(batch, label, px.x, px.y + (px.h - font.Height()) * 0.5f,
			  theme.textDim);

	const gfx::Rect swatch = SwatchRect();
	batch.DrawRect(swatch, {0, 0, 0, 1}); // opaque base so alpha reads as darkness
	batch.DrawRect(swatch, m_color);
	DrawBorder(batch, swatch, m_hot || m_open ? theme.accent : theme.panelBorder);
}

void ColorPicker::DrawOverlaySelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_open) return;
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect popup = PopupRect(ctx);

	if (const Skin* skin = PanelSkin(ctx)) {
		DrawFace(batch, popup, *skin, Face::Panel, {1, 1, 1, 1}); // opaque, unlike Panel
	} else {
		Vec4 background = theme.panel;
		background.w = 1.0f; // opaque so the page beneath doesn't bleed through
		batch.DrawRect(popup, background);
		DrawBorder(batch, popup, theme.panelBorder);
	}

	static constexpr const char* kChannelNames[4] = {"R", "G", "B", "A"};
	static constexpr Vec4 kChannelTints[4] = {{0.9f, 0.3f, 0.3f, 1.0f},
											  {0.3f, 0.85f, 0.3f, 1.0f},
											  {0.35f, 0.55f, 1.0f, 1.0f},
											  {0.8f, 0.8f, 0.8f, 1.0f}};
	for (int i = 0; i < 4; ++i) {
		const gfx::Rect row = PickerRow(popup, i, Rem());
		const gfx::Rect track = PickerTrack(row, Rem());
		const float value = Channel(m_color, i);
		const float textY = row.y + (row.h - font.Height()) * 0.5f;

		font.Draw(batch, kChannelNames[i], row.x, textY, theme.textDim);

		const float trackH = Rem(0.15f);
		const float trackY = track.y + (track.h - trackH) * 0.5f;
		batch.DrawRect({track.x, trackY, track.w, trackH}, theme.control);
		batch.DrawRect({track.x, trackY, track.w * value, trackH}, kChannelTints[i]);
		// Slider's proportions: half the height, twice the width it had.
		const float tw = Rem(0.6f);
		const float th = (track.h - Rem(0.16f)) * 0.5f;
		const gfx::Rect thumb{track.x + track.w * value - tw * 0.5f,
							  trackY + trackH * 0.5f - th * 0.5f, tw, th};
		batch.DrawRect(thumb,
					   m_dragChannel == i ? theme.controlActive : theme.controlHot);
		DrawBorder(batch, thumb, theme.panelBorder);

		font.Draw(batch, std::format("{}", static_cast<int>(value * 255.0f + 0.5f)),
				  track.x + track.w + Rem(0.35f), textY, theme.text);
	}
}

// --- KeyBind -----------------------------------------------------------------

KeyBind::KeyBind(const gfx::Rect& rect, std::string label, int vkey,
				 std::function<void(int)> onChange)
	: label(std::move(label)), onChange(std::move(onChange)) {
	bounds = rect;
	SetKey(vkey);
}

void KeyBind::SetKey(int vkey) {
	m_vkey = vkey & 0xFF;
	m_keyName = KeyName(m_vkey);
}

gfx::Rect KeyBind::BoxRect() const {
	const gfx::Rect& px = Pixel();
	const float w = std::min(Rem(7.1f), px.w * 0.45f);
	return {px.x + px.w - w, px.y, w, px.h};
}

void KeyBind::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;

	if (m_capturing) {
		if (input->WasMousePressed(MouseButton::Left) ||
			input->WasKeyPressed(vk::Escape)) {
			m_capturing = false; // any click (incl. the box) or Esc cancels
		} else if (const int vkey = input->FirstPressedKey(); vkey >= 0) {
			m_capturing = false;
			SetKey(vkey);
			if (onChange) onChange(m_vkey);
		}
		ctx.ConsumeMouse(); // the armed box owns the mouse entirely
		return;
	}

	m_hot = !ctx.IsMouseConsumed() &&
			BoxRect().Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_capturing = true;
	}
}

void KeyBind::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	font.Draw(batch, label, px.x, px.y + (px.h - font.Height()) * 0.5f,
			  theme.textDim);

	const gfx::Rect box = BoxRect();
	DrawFieldFace(ctx, batch, box,
				  m_capturing ? FieldState::Active : (m_hot ? FieldState::Hot : FieldState::Normal),
				  m_capturing ? theme.controlActive : (m_hot ? theme.controlHot : theme.control),
				  m_capturing || m_hot ? theme.accent : theme.panelBorder);

	const std::string& text = m_capturing ? capturePrompt : m_keyName;
	const float textW = font.MeasureWidth(text);
	font.Draw(batch, text, box.x + (box.w - textW) * 0.5f,
			  box.y + (box.h - font.Height()) * 0.5f,
			  m_capturing ? theme.accent : theme.text);
}

// --- TextField -------------------------------------------------------------

void TextField::UpdateSelf(UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;

	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(input->MouseX(), input->MouseY());
	if (input->WasMousePressed(MouseButton::Left)) {
		if (m_hot) {
			m_focused = true;
			ctx.ConsumeMouse();
		} else if (!Pixel().Contains(input->MouseX(), input->MouseY())) {
			m_focused = false; // a click elsewhere drops focus (don't consume it)
		}
	} else if (m_hot) {
		ctx.ConsumeMouse();
	}
	if (!m_focused) return;

	// The typed text in order (Input::TypedChars): Backspace where it fell, and
	// an Enter ends this frame's typing - submitting usually closes whatever the
	// field is on, so nothing typed after it is applied to the old text.
	// WHOLE CHARACTERS (C383): Backspace takes the last character however many
	// bytes it is, and maxLength counts characters, so a full field refuses a
	// letter whole rather than keeping half of it.
	bool changed = false;
	bool submit = false;
	const std::string_view typed = input->TypedChars();
	for (size_t i = 0; i < typed.size();) {
		const std::string_view ch = utf8::CharAt(typed, i);
		i += ch.size();
		if (ch[0] == Input::kTypedEnter) {
			submit = true;
			break;
		}
		if (ch[0] == Input::kTypedBack) {
			if (!utf8::PopBack(text)) continue;
		} else {
			if (utf8::Length(text) >= maxLength) continue;
			text.append(ch);
		}
		changed = true;
	}
	if (submit && onSubmit) onSubmit();
	if (changed && onChange) onChange();
}

void TextField::DrawSelf(UIContext& ctx, gfx::SpriteBatch& batch) {
	const Theme& theme = ctx.GetTheme();
	const Font& font = TextFont();
	const gfx::Rect& px = Pixel();

	const gfx::Rect face = DrawFieldFace(
		ctx, batch, px,
		m_focused ? FieldState::Active : (m_hot ? FieldState::Hot : FieldState::Normal),
		m_focused ? theme.controlActive : (m_hot ? theme.controlHot : theme.control),
		m_focused || m_hot ? theme.accent : theme.panelBorder);

	const float pad = Rem(0.3f);
	const float ty = px.y + (px.h - font.Height()) * 0.5f;
	// Text is CLIPPED to the field, and scrolled to follow the caret. Without
	// this a value wider than the box simply paints past its own border: the
	// control was fine for as long as every field held a short number, and the
	// first long value put in one (a list of theme tags) spilled ink outside the
	// control and over the panel edge. That breaks the rule the whole layout
	// rests on — a widget's area is its own — and `uioverlap` scores it as an
	// escape.
	// Inside the face's frame (the flat face returns the whole rect, so that is
	// the old 1px inset).
	const gfx::Rect inner{face.x + 1.0f, face.y + 1.0f, face.w - 2.0f, face.h - 2.0f};
	ScopedClip clip(batch, inner);
	if (text.empty() && !m_focused) {
		font.Draw(batch, placeholder, px.x + pad, ty, theme.textDim);
	} else {
		// FOCUSED: slide left so the caret stays in view — you have to be able to
		// see what you are typing. UNFOCUSED: show the START of the value, which
		// is the part that identifies it, and snapping back on blur is what every
		// other text field does.
		const float textW = font.MeasureWidth(text);
		const float avail = std::max(px.w - pad * 2.0f, 0.0f);
		const float scroll = m_focused ? std::max(0.0f, textW - avail) : 0.0f;
		font.Draw(batch, text, px.x + pad - scroll, ty, theme.text);
		if (m_focused) {
			const float caretX = px.x + pad - scroll + textW + 1.0f;
			// 2px caret is a hairline and stays one (Units.h).
			batch.DrawRect({caretX, px.y + Rem(0.22f), 2.0f, px.h - Rem(0.44f)},
						   theme.accent);
		}
	}
}

} // namespace dungeon::ui
