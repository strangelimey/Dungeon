// ============================================================================
// Game/DialogLayout.cpp — see DialogLayout.h.
// ============================================================================
#include "Game/DialogLayout.h"

namespace dungeon::game {

namespace {
// All in rem (UI/Units.h), all folding in the scale these dialogs draw at.
constexpr float kPagePad = 1.3f;    // panel edge to content
constexpr float kPageGap = 0.5f;    // title / body / footer
constexpr float kTitleRow = ui::kDialogTitleScale * 1.35f;
constexpr float kCloseSlot = ui::kDialogTitleScale * 0.85f;
constexpr float kFooterRow = ui::kDialogTextScale * 1.9f;
constexpr float kFooterBtn = ui::kDialogTextScale * 4.6f;
} // namespace

EditableTitle::EditableTitle(const gfx::Rect& rect, std::string prefix,
							 std::string name, std::function<void()> onClick)
	: prefix(std::move(prefix)), name(std::move(name)),
	  onClick(std::move(onClick)) {
	bounds = rect;
	fontScale = ui::kDialogTitleScale;
}

gfx::Rect EditableTitle::NamePixels() const {
	const ui::Font& font = TextFont();
	const gfx::Rect& px = Pixel();
	const float h = font.Height();
	return {px.x + font.MeasureWidth(prefix), px.y + (px.h - h) * 0.5f,
			font.MeasureWidth(name), h};
}

gfx::Rect EditableTitle::InkRect() const {
	const gfx::Rect n = NamePixels();
	return {Pixel().x, n.y, n.x + n.w - Pixel().x, n.h + 2.0f};
}

void EditableTitle::UpdateSelf(ui::UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	m_hot = false;
	if (!input || ctx.IsMouseConsumed()) return;
	m_hot = NamePixels().Contains(input->MouseX(), input->MouseY());
	if (m_hot && input->WasMousePressed(MouseButton::Left)) {
		ctx.ConsumeMouse();
		if (onClick) onClick();
	}
}

void EditableTitle::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& th = ctx.GetTheme();
	const ui::Font& font = TextFont();
	const gfx::Rect n = NamePixels();
	font.Draw(batch, prefix, Pixel().x, n.y, th.text);
	font.Draw(batch, name, n.x, n.y, m_hot ? th.accent : th.text);
	// The hint underline: this half is clickable, the prefix is not.
	batch.DrawRect({n.x, n.y + n.h + 1.0f, n.w, 1.0f},
				   m_hot ? th.accent : th.textDim);
}

void PreviewPane::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const gfx::Rect& px = Pixel();
	batch.DrawRect(px, {0.02f, 0.02f, 0.03f, 1.0f});
	if (border) ui::DrawBorder(batch, px, ctx.GetTheme().panelBorder);
	if (hint.empty() || !showHint) return;
	const ui::Font& font = TextFont();
	font.Draw(batch, hint, px.x + (px.w - font.MeasureWidth(hint)) * 0.5f,
			  px.y + (px.h - font.Height()) * 0.5f, ctx.GetTheme().textDim);
}

ui::Len FormRow(float lines) {
	return ui::Len::Fixed(ui::kDialogTextScale * 1.6f * lines);
}

ui::Len FooterButton(float widths) {
	return ui::Len::Fixed(kFooterBtn * widths);
}

DialogChrome BuildDialogChrome(ui::UIContext& ui, const gfx::Rect& panel,
							   const std::string& title,
							   const gfx::Texture* closeIcon,
							   std::function<void()> onClose, bool withFooter) {
	DialogChrome c;
	c.page = ui.Add<ui::Stack>(panel);
	c.page->debugName = "page";
	c.page->padRem = kPagePad;
	c.page->gapRem = kPageGap;

	// Title row: the title beside a slot HOLDING the close box. Reserving the
	// slot and then floating the button over the panel corner would leave the
	// two free to disagree — and did, until the overlap audit said so.
	c.titleRow = c.page->Row<ui::Stack>(ui::Len::Fixed(kTitleRow), true);
	c.titleRow->debugName = "title";
	c.titleSlot = c.titleRow->Space(ui::Len::Fill());
	c.titleSlot->debugName = "title-slot";
	if (!title.empty()) {
		c.title = c.titleSlot->Add<ui::Label>(gfx::Rect{0, 0, 1, 1}, title);
		c.title->fontScale = ui::kDialogTitleScale;
		c.title->centerV = true;
	}
	ui::Box* closeSlot = c.titleRow->Space(ui::Len::Fixed(kCloseSlot));
	closeSlot->debugName = "close";
	ui::AddCloseButton(*closeSlot, closeIcon, std::move(onClose));

	c.body = c.page->Row<ui::Stack>(ui::Len::Fill());
	c.body->debugName = "body";
	c.body->gapRem = 0.5f;

	if (withFooter) {
		c.footer = c.page->Row<ui::Stack>(ui::Len::Fixed(kFooterRow), true);
		c.footer->debugName = "footer";
		c.footer->gapRem = 0.6f;
	}
	return c;
}

} // namespace dungeon::game
