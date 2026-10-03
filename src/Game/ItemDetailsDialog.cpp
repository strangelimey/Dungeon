// ============================================================================
// Game/ItemDetailsDialog.cpp - see ItemDetailsDialog.h.
// ============================================================================
#include "Game/ItemDetailsDialog.h"

#include "Core/Loc.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "Game/PartyHudDraw.h" // the rune groove's breath (kRuneGroove*)
#include "UI/Controls.h"
#include "UI/Layout.h"
#include "UI/TextWrap.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace dungeon::game {

namespace {

// The card, in window fractions. Wide rather than tall: the model and the text
// sit side by side.
constexpr gfx::Rect kPanel{0.19f, 0.14f, 0.62f, 0.66f};
// The detail lines read a size down from the dialog's text, so a weapon's seven
// of them still leave the description its room. `kLineScale` is the lines'
// font scale (the root is ui::kDialogTextScale); FormRow wants it in LINES of
// that root, hence the ratio.
constexpr float kLineScale = 1.6f;
constexpr float kLineRows = kLineScale / ui::kDialogTextScale;
// The body's split: the turning model left, the lines right.
constexpr float kPaneFill = 0.42f;
constexpr float kColumnFill = 0.58f;
constexpr float kGutterRem = 1.0f;
// Within a line, the label and its value. The value takes most of it: the
// labels are single short words, and a plate cuirass's three resistances are
// one long line ("Slash +50%, Pierce +40%, Bash +20%").
constexpr float kLabelFill = 0.6f;
constexpr float kValueFill = 1.5f;
// One slow full turn, the way a thing is turned over in the hand to look at it.
constexpr float kSpinSeconds = 20.0f;
// Room reserved in each assigned string, so opening never grows one.
constexpr size_t kValueCap = 160;
constexpr size_t kDescCap = 600;

// `v` as tenths ("1.5"), formatted as integers - MSVC's float precision path
// allocates in debug (the carry-load line's reason).
std::string_view Tenths(std::span<char> buf, float v) {
	const long t = std::lround(v * 10.0f);
	const long a = t < 0 ? -t : t;
	const auto end =
		std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()), "{}{}.{}",
						 t < 0 ? "-" : "", a / 10, a % 10)
			.out;
	return {buf.data(), static_cast<size_t>(end - buf.data())};
}

// "<prefix><id><suffix>" looked up, copied into a Line (a missing key comes back
// as the key, which here is a stack buffer). `missing` says whether it was.
loc::Line Lookup(std::string_view prefix, std::string_view id, std::string_view suffix,
				 bool* missing = nullptr) {
	char key[128];
	size_t n = 0;
	for (std::string_view part : {prefix, id, suffix}) {
		const size_t k = std::min(part.size(), sizeof(key) - n);
		std::copy_n(part.data(), k, key + n);
		n += k;
	}
	const std::string_view k(key, n);
	const std::string_view text = loc::View(k);
	if (missing) *missing = text == k;
	return loc::Line(text);
}

} // namespace

// The description: in-world prose, wrapped to the column and set in the Script
// face like a spell's or an effect's on the sheet. A Label does not wrap.
class DescriptionText : public ui::Widget {
public:
	explicit DescriptionText(const gfx::Rect& rect) {
		bounds = rect;
		debugName = "description";
		text.reserve(kDescCap);
	}
	std::string text;

private:
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override {
		const ui::Font& font = TextFont();
		const gfx::Rect& r = Pixel();
		const float bottom = r.y + r.h;
		ui::WrapLines(font, text, r.w, [&](std::string_view line, int n) {
			const float y = r.y + static_cast<float>(n) * font.LineAdvance();
			if (y + font.Height() <= bottom)
				font.Draw(batch, line, r.x, y, ctx.GetTheme().textDim);
		});
	}
};

ItemDetailsDialog::ItemDetailsDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	m_ui.Root().fontScale = ui::kDialogTextScale; // the editor dialogs' reading size
	m_closeIcon = CloseIcon(device);
	Build();
}

ItemDetailsDialog::~ItemDetailsDialog() = default;

void ItemDetailsDialog::Build() {
	m_ui.Clear();
	m_rows = {};
	m_memorize = nullptr;
	DialogChrome chrome = BuildDialogChrome(m_ui, kPanel, " ", m_closeIcon, [this] { Close(); });
	m_title = chrome.title;
	if (m_title) m_title->text.reserve(kValueCap);

	// The footer holds the one action an item can be given from here: a rune
	// MEMORIZED (right-aligned, the dialogs' footer convention). Hidden until the
	// caller says the holder can use it.
	chrome.footer->Space(ui::Len::Fill());
	m_memorize = chrome.footer->Row<ui::Button>(FooterButton(1.6f), loc::Tr("use.memorize"),
												[this] {
													if (onMemorize) onMemorize();
												});
	m_memorize->visible = false;

	ui::Stack* body = chrome.body;
	body->horizontal = true;
	m_pane = body->Row<PreviewPane>(ui::Len::Fill(kPaneFill));
	m_pane->debugName = "preview";
	body->Space(ui::Len::Fixed(kGutterRem));
	ui::Stack* column = body->Row<ui::Stack>(ui::Len::Fill(kColumnFill));
	column->debugName = "details";
	column->gapRem = 0.1f;

	static constexpr const char* kLabels[kRowCount] = {
		"item.detail.category", "item.detail.weight",  "item.detail.damage",
		"item.detail.speed",    "item.detail.skill",   "item.detail.reach",
		"item.detail.element",  "item.detail.armor",   "item.detail.armorclass",
		"item.detail.worn",     "item.detail.resists", "item.detail.nutrition",
		"item.detail.hydration", "item.detail.restore_health",
		"item.detail.restore_stamina", "item.detail.restore_mana", "item.detail.cures"};
	for (size_t i = 0; i < kRowCount; ++i) {
		ui::Stack* row = column->Row<ui::Stack>(FormRow(kLineRows), true);
		row->debugName = "line";
		row->fontScale = kLineScale;
		ui::Label* label = row->Row<ui::Label>(ui::Len::Fill(kLabelFill), loc::Tr(kLabels[i]));
		label->dim = true;
		label->centerV = true;
		ui::Label* value = row->Row<ui::Label>(ui::Len::Fill(kValueFill), std::string{});
		value->centerV = true;
		value->text.reserve(kValueCap);
		m_rows[i] = {row, value};
	}
	column->Row<ui::Separator>(ui::Len::Fixed(0.6f));
	m_desc = column->Row<DescriptionText>(ui::Len::Fill());
	m_desc->fontRole = ui::FontRole::Script;
	m_desc->fontScale = 1.15f;
}

void ItemDetailsDialog::SetRow(RowId id, std::string_view text) {
	Row& row = m_rows[static_cast<size_t>(id)];
	if (!row.row || !row.value) return;
	row.row->visible = !text.empty();
	row.value->text.assign(text);
}

void ItemDetailsDialog::SetPreview(size_t count, const Vec3& fitMin, const Vec3& fitMax,
								   const Mat4& pose) {
	m_subCount = std::min(count, m_subs.size());
	m_fitMin = fitMin;
	m_fitMax = fitMax;
	m_pose = pose;
}

void ItemDetailsDialog::Open(const ItemDetails& d, float weightKg) {
	if (m_title) m_title->text.assign(loc::View(d.nameKey));
	m_burning = d.burning;
	m_flameHead = d.flameHead;
	char a[32], b[32];

	SetRow(kCategory, d.category.empty() ? std::string_view{}
										 : Lookup("itemcat.", d.category, "").View());
	SetRow(kWeight, loc::FormatLine("sheet.status.weight", Tenths(a, weightKg)).View());

	// A weapon: what it deals, how fast, what it trains, where it swings from,
	// and any element it carries. An item with no damage of its own swings as a
	// bare hand, so it has none of these lines.
	const bool weapon = d.damage > 0.0f;
	SetRow(kDamage, weapon ? std::string_view(Tenths(a, d.damage)) : std::string_view{});
	SetRow(kSpeed, weapon && d.speed > 0.0f
					   ? loc::FormatLine("item.detail.seconds", Tenths(b, d.speed)).View()
					   : std::string_view{});
	SetRow(kSkill, weapon && !d.skill.empty() ? Lookup("skill.", d.skill, "").View()
											  : std::string_view{});
	SetRow(kReach, weapon ? loc::View(d.polearm ? "item.detail.reach.polearm"
											   : "item.detail.reach.melee")
						  : std::string_view{});
	SetRow(kElement, d.enchanted
						 ? loc::FormatLine("item.detail.element_value",
										   loc::View(SymbolKey(d.element)),
										   static_cast<int>(std::lround(d.elementBonus * 100.0f)))
							   .View()
						 : std::string_view{});

	// Protection: the soak, the weight class (named after the skill it trains,
	// which is what the player sees on the sheet), where it is worn, and its
	// resistances as one line.
	SetRow(kArmor, d.armor > 0.0f ? std::string_view(Tenths(a, d.armor)) : std::string_view{});
	SetRow(kArmorClass, d.armorClass != ArmorClass::None
							? Lookup("skill.", ArmorSkillId(d.armorClass), "").View()
							: std::string_view{});
	SetRow(kWorn, d.wear != WearSlot::None ? Lookup("wear.", WearSlotId(d.wear), "").View()
										   : std::string_view{});
	char resists[loc::Line::kCapacity];
	size_t n = 0;
	for (size_t i = 0; i < d.resistCount; ++i) {
		const ItemDetails::Resist& r = d.resists[i];
		const std::string_view name = loc::View(r.nameKey);
		const auto end = std::format_to_n(
			resists + n, static_cast<std::ptrdiff_t>(sizeof(resists) - n), "{}{} {}{}%",
			i ? ", " : "", name, r.value > 0.0f ? "+" : "",
			static_cast<int>(std::lround(r.value * 100.0f)));
		n = std::min(static_cast<size_t>(end.out - resists), sizeof(resists));
	}
	SetRow(kResists, std::string_view(resists, n));

	// Food and drink.
	SetRow(kNutrition, d.nutrition > 0.0f ? std::string_view(Tenths(a, d.nutrition))
										  : std::string_view{});
	SetRow(kHydration, d.hydration > 0.0f ? std::string_view(Tenths(b, d.hydration))
										  : std::string_view{});

	// A potion: what it restores at once, and what it treats - an effect by its
	// own name, with the share of its bite taken away when that is not all of it.
	char h[32], s[32], m[32];
	SetRow(kRestoreHealth, d.restoreHealth > 0.0f ? std::string_view(Tenths(h, d.restoreHealth))
												  : std::string_view{});
	SetRow(kRestoreStamina, d.restoreStamina > 0.0f
								? std::string_view(Tenths(s, d.restoreStamina))
								: std::string_view{});
	SetRow(kRestoreMana, d.restoreMana > 0.0f ? std::string_view(Tenths(m, d.restoreMana))
											  : std::string_view{});
	char cures[loc::Line::kCapacity];
	size_t c = 0;
	for (size_t i = 0; i < d.cureCount; ++i) {
		const ItemDetails::Cure& cure = d.cures[i];
		const loc::Line name = Lookup("effect.", cure.effect, "");
		const auto end =
			cure.share >= 1.0f
				? std::format_to_n(cures + c, static_cast<std::ptrdiff_t>(sizeof(cures) - c),
								   "{}{}", i ? ", " : "", name.View())
				: std::format_to_n(cures + c, static_cast<std::ptrdiff_t>(sizeof(cures) - c),
								   "{}{} {}%", i ? ", " : "", name.View(),
								   static_cast<int>(std::lround(cure.share * 100.0f)));
		c = std::min(static_cast<size_t>(end.out - cures), sizeof(cures));
	}
	SetRow(kCures, std::string_view(cures, c));

	// item.<id>.desc by the name key's convention; an item without one simply has
	// no paragraph rather than printing its key.
	if (m_desc) {
		bool missing = false;
		const loc::Line desc = Lookup(d.nameKey, "", ".desc", &missing);
		m_desc->text.assign(missing ? std::string_view{} : desc.View());
	}
	m_spin = 0.0f;
	ShowMemorize(false); // the caller shows it, for a rune its holder can learn
	m_open = true;
	++m_opens;
}

void ItemDetailsDialog::ShowMemorize(bool shown) {
	if (m_memorize) m_memorize->visible = shown;
}

bool ItemDetailsDialog::MemorizeShown() const { return m_memorize && m_memorize->visible; }

void ItemDetailsDialog::Update(const Input& input, float w, float h, float dt) {
	if (!m_open) return;
	m_ui.UseFont(ui::FontRole::Body, std::clamp(h * 0.020f, 12.0f, 24.0f));
	constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
	m_spin = std::fmod(m_spin + dt * (kTwoPi / kSpinSeconds), kTwoPi);
	// A rune tablet's groove breathes as its halo does in the pack (the same
	// colour and the same 3.4 s breath), round the mean its icon holds.
	m_breath = std::fmod(m_breath + dt * (kTwoPi / kRuneBreathSeconds), kTwoPi);
	for (size_t i = 0; i < m_subCount; ++i)
		if (m_subs[i].material.emissiveGroove > 0.0f)
			m_subs[i].material.emissiveGroove =
				kRuneGrooveMean + kRuneGrooveSwing * std::sin(m_breath);
	m_ui.Update(input, w, h);
}

void ItemDetailsDialog::Render(gfx::SpriteBatch& batch, const ui::Theme&, float w, float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.5f});
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	// An OPAQUE backing under the panel face: the face is translucent (it lets
	// the scene through behind the sheet), and over the sheet that put two pages
	// of text on top of each other.
	const ui::Theme& theme = m_ui.GetTheme();
	batch.DrawRect(panel, {theme.panel.x, theme.panel.y, theme.panel.z, 1.0f});
	ui::DrawPanelFace(m_ui, batch, panel);
	m_ui.Render(batch, w, h);
}

gfx::Rect ItemDetailsDialog::PreviewRect() const {
	return m_pane ? m_pane->Pixel() : gfx::Rect{};
}

} // namespace dungeon::game
