// ============================================================================
// Game/PortraitPicker.cpp - see PortraitPicker.h.
// ============================================================================
#include "Game/PortraitPicker.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/Catalog.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"
#include "UI/Layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace dungeon::game {

namespace {

// The card, in window fractions: tall and wide, since the point is to see many
// faces at once.
constexpr gfx::Rect kPanel{0.14f, 0.07f, 0.72f, 0.86f};
// A tile is a square about this many rem across; the grid fits as many columns
// as that allows and stretches them to fill the width.
constexpr float kTileRem = 6.5f;
constexpr float kTileGapRem = 0.4f;
// Thumbnails: the .dds mips at or under this size (the 128 level of a 256
// portrait; Corax's 512s drop two levels), loaded a few a frame - each load
// drains the GPU, and a screenful at once is a visible stall.
constexpr u32 kThumbPx = 128;
constexpr size_t kThumbLoadsPerFrame = 3;
// Tile images kept: a screenful is ~60, so this holds a few screens of
// scrolling back and forth and stays far inside the 1024-slot SRV heap.
constexpr size_t kThumbCap = 240;
constexpr size_t kTextCap = 96; // reserved in the title and the count

// The tag vocabularies, in the order the drop-downs list them (after Any). The
// words are portraits.cat's (tools/BuildPortraitCatalog.py); each is localized
// as portrait.<field>.<word>. `unclear` is in none of them: such a portrait
// only shows under Any.
constexpr std::array<const char*, 7> kRaces = {"human", "elf",    "dwarf", "orc",
											   "undead", "demon", "other"};
constexpr std::array<const char*, 2> kSexes = {"male", "female"};
constexpr std::array<const char*, 3> kAges = {"young", "adult", "old"};

int IndexOf(std::span<const char* const> words, std::string_view word) {
	for (size_t i = 0; i < words.size(); ++i)
		if (word == words[i]) return static_cast<int>(i);
	return -1;
}

} // namespace

// --- the grid ----------------------------------------------------------------

// Every shown portrait, as one widget. It sizes its bounds to ALL of its rows,
// which is what the ScrollArea around it reads to know how far to scroll (the
// fitContent idea, UI/Layout.h), and it draws and hit-tests only the rows inside
// the area's view - the rest cost nothing.
class PortraitGrid : public ui::Widget {
public:
	PortraitGrid(const gfx::Rect& rect, PortraitPicker& owner, ui::ScrollArea& area)
		: m_owner(owner), m_area(area) {
		bounds = rect;
		debugName = "portrait-grid";
	}

	// The shown indices inside the view: [first, first + count).
	void Visible(size_t& first, size_t& count) const {
		first = count = 0;
		const size_t n = m_owner.m_shown.size();
		const float pitch = m_tile + m_gap;
		if (n == 0 || pitch <= 0.0f) return;
		const gfx::Rect view = m_area.ViewRect();
		const float top = view.y - Pixel().y;
		const size_t r0 = top <= 0.0f ? 0 : static_cast<size_t>(top / pitch);
		const size_t r1 =
			static_cast<size_t>(std::ceil(std::max(top + view.h, 0.0f) / pitch));
		first = std::min(r0 * m_cols, n);
		count = std::min(r1 * m_cols, n) - first;
	}
	// How far down the grid a shown index's row starts.
	float RowOffset(size_t shownIndex) const {
		return static_cast<float>(shownIndex / m_cols) * (m_tile + m_gap);
	}
	float Extent() const { return m_extent; }
	float Tile() const { return m_tile; }

protected:
	void LayoutSelf(ui::UIContext&) override {
		const gfx::Rect& px = Pixel();
		m_gap = Rem(kTileGapRem);
		const float target = Rem(kTileRem);
		m_cols = std::max<size_t>(1, static_cast<size_t>((px.w + m_gap) / (target + m_gap)));
		const float cols = static_cast<float>(m_cols);
		m_tile = std::max((px.w - m_gap * (cols - 1.0f)) / cols, 1.0f);
		const size_t rows = (m_owner.m_shown.size() + m_cols - 1) / m_cols;
		m_extent = rows ? static_cast<float>(rows) * m_tile +
							  static_cast<float>(rows - 1) * m_gap
						: 0.0f;
		const gfx::Rect& box = ContainerRect();
		if (box.h > 0.0f) bounds.h = std::max(m_extent / box.h, 0.001f);
	}

	void UpdateSelf(ui::UIContext& ctx) override {
		m_hot = -1;
		const Input* input = ctx.CurrentInput();
		if (!input || ctx.IsMouseConsumed()) return;
		const float mx = input->MouseX(), my = input->MouseY();
		// Only what the view shows can be pointed at: input is clipped like
		// drawing (a scrolled-out row is not hot).
		if (!m_area.ViewRect().Contains(mx, my)) return;
		const int i = IndexAt(mx, my);
		if (i < 0) return;
		m_hot = i;
		ctx.ConsumeMouse(); // the pointer, not the wheel - that is the area's
		if (input->WasMousePressed(MouseButton::Left))
			m_owner.Pick(static_cast<size_t>(i)); // closes the picker
	}

	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override {
		const ui::Theme& th = ctx.GetTheme();
		size_t first = 0, count = 0;
		Visible(first, count);
		for (size_t i = first; i < first + count; ++i) {
			const gfx::Rect r = TileRect(i);
			if (const gfx::Texture* tex = m_owner.ThumbFor(i))
				batch.DrawSprite(r, {0, 0, 1, 1}, *tex, {1, 1, 1, 1});
			else
				batch.DrawRect(r, th.control); // still loading (or missing)
			if (static_cast<int>(i) == m_hot) {
				batch.DrawRect(r, {1, 1, 1, 0.12f});
				ui::DrawBorder(batch, r, th.text);
			}
			if (m_owner.IdAt(i) == m_owner.m_current) { // two hairlines: it must show
				ui::DrawBorder(batch, r, th.accent);
				ui::DrawBorder(batch, {r.x + 1, r.y + 1, r.w - 2, r.h - 2}, th.accent);
			}
		}
	}

private:
	gfx::Rect TileRect(size_t i) const {
		const gfx::Rect& px = Pixel();
		const float pitch = m_tile + m_gap;
		return {px.x + static_cast<float>(i % m_cols) * pitch,
				px.y + static_cast<float>(i / m_cols) * pitch, m_tile, m_tile};
	}
	// The shown index under a point, or -1 (a gap, or past the last tile).
	int IndexAt(float x, float y) const {
		const gfx::Rect& px = Pixel();
		const float pitch = m_tile + m_gap;
		if (pitch <= 0.0f || x < px.x || y < px.y) return -1;
		const size_t col = static_cast<size_t>((x - px.x) / pitch);
		const size_t row = static_cast<size_t>((y - px.y) / pitch);
		if (col >= m_cols) return -1;
		if (x - px.x - static_cast<float>(col) * pitch > m_tile ||
			y - px.y - static_cast<float>(row) * pitch > m_tile)
			return -1;
		const size_t i = row * m_cols + col;
		return i < m_owner.m_shown.size() ? static_cast<int>(i) : -1;
	}

	PortraitPicker& m_owner;
	ui::ScrollArea& m_area;
	size_t m_cols = 1;
	float m_tile = 0.0f, m_gap = 0.0f, m_extent = 0.0f;
	int m_hot = -1;
};

// --- the picker --------------------------------------------------------------

PortraitPicker::PortraitPicker(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f), m_thumbs(device, kThumbCap) {
	m_ui.Root().fontScale = ui::kDialogTextScale; // the editor dialogs' reading size
	m_closeIcon = CloseIcon(device);
	Build();
}

PortraitPicker::~PortraitPicker() = default;

void PortraitPicker::Build() {
	m_ui.Clear();
	m_title = m_count = nullptr;
	m_raceDrop = m_sexDrop = m_ageDrop = nullptr;
	m_area = nullptr;
	m_grid = nullptr;

	DialogChrome chrome = BuildDialogChrome(m_ui, kPanel, " ", m_closeIcon,
											[this] { Close(); }, /*withFooter=*/false);
	m_title = chrome.title;
	if (m_title) m_title->text.reserve(kTextCap);

	ui::Stack* body = chrome.body;
	body->gapRem = 0.5f;
	ui::Stack* filters = body->Row<ui::Stack>(FormRow(), true);
	filters->debugName = "filters";
	filters->gapRem = 0.5f;
	const auto addFilter = [&](const char* labelKey, std::string_view field,
							   std::span<const char* const> words, int& state) {
		ui::Label* label = filters->Row<ui::Label>(ui::Len::Fill(0.45f), loc::Tr(labelKey));
		label->centerV = true;
		label->dim = true;
		std::vector<std::string> items{loc::Tr("portrait.pick.any")};
		for (const char* w : words)
			items.push_back(loc::Tr(std::string("portrait.") + std::string(field) + "." + w));
		return filters->Row<ui::DropDown>(ui::Len::Fill(1.0f), std::move(items), state,
										  [this, &state](int i) {
											  state = i;
											  ApplyFilter();
										  });
	};
	m_raceDrop = addFilter("portrait.pick.race", "race", kRaces, m_race);
	m_sexDrop = addFilter("portrait.pick.sex", "sex", kSexes, m_sex);
	m_ageDrop = addFilter("portrait.pick.age", "age", kAges, m_age);
	m_count = filters->Row<ui::Label>(ui::Len::Fill(1.0f), std::string{});
	m_count->centerV = true;
	m_count->dim = true;
	m_count->text.reserve(kTextCap);

	m_area = body->Row<ui::ScrollArea>(ui::Len::Fill());
	m_area->debugName = "portraits";
	m_grid = m_area->Add<PortraitGrid>(gfx::Rect{0, 0, 1, 1}, *this, *m_area);
	ApplyFilter();
}

void PortraitPicker::SetCatalog(const Catalog& portraits) {
	if (m_open) Close();
	m_entries.clear();
	m_entries.reserve(portraits.Entries().size());
	for (const CatalogEntry& e : portraits.Entries())
		m_entries.push_back({e.id, IndexOf(kRaces, e.Get("race")), IndexOf(kSexes, e.Get("sex")),
							 IndexOf(kAges, e.Get("age"))});
	m_shown.reserve(m_entries.size()); // so a filter change never grows it
	m_race = m_sex = m_age = 0;
	if (m_raceDrop) m_raceDrop->SetSelected(0);
	if (m_sexDrop) m_sexDrop->SetSelected(0);
	if (m_ageDrop) m_ageDrop->SetSelected(0);
	ApplyFilter();
}

void PortraitPicker::ApplyFilter() {
	m_shown.clear();
	for (size_t i = 0; i < m_entries.size(); ++i) {
		const Entry& e = m_entries[i];
		if (m_race > 0 && e.race != m_race - 1) continue;
		if (m_sex > 0 && e.sex != m_sex - 1) continue;
		if (m_age > 0 && e.age != m_age - 1) continue;
		m_shown.push_back(i);
	}
	if (m_area) m_area->ScrollToTop(); // a new result starts at the top
	if (m_count)
		m_count->text.assign(m_shown.empty()
								 ? loc::View("portrait.pick.none")
								 : loc::FormatLine("portrait.pick.count", m_shown.size()).View());
}

void PortraitPicker::Open(std::string_view title, const std::string& currentId,
						  std::function<void(const std::string&)> onPick) {
	if (m_title) m_title->text.assign(title);
	m_current = currentId;
	m_onPick = std::move(onPick);
	// Every open starts unfiltered, so the current portrait is in the grid to be
	// seen (scrolled to, below) whatever the last open was filtered on.
	SetFilter(0, 0, 0);
	m_scrollToCurrent = true;
	m_scrollFraction = -1.0f;
	m_open = true;
}

void PortraitPicker::Close() {
	m_open = false;
	m_onPick = nullptr;
	// Give the SRV slots back: a closed picker has no business holding a few
	// hundred thumbnails. Clear drains first (the SRV rule).
	m_thumbs.Clear();
}

void PortraitPicker::Pick(size_t shownIndex) {
	const std::string id = IdAt(shownIndex);
	auto pick = std::move(m_onPick);
	Close();
	if (pick && !id.empty()) pick(id);
}

const std::string& PortraitPicker::IdAt(size_t shownIndex) const {
	static const std::string kNone;
	return shownIndex < m_shown.size() ? m_entries[m_shown[shownIndex]].id : kNone;
}

const gfx::Texture* PortraitPicker::ThumbFor(size_t shownIndex) {
	const std::string& id = IdAt(shownIndex);
	return id.empty() ? nullptr : m_thumbs.Touch(id).data.texture.get();
}

void PortraitPicker::LoadVisibleThumbs(size_t max) {
	if (!m_grid) return;
	size_t first = 0, count = 0, loaded = 0;
	m_grid->Visible(first, count);
	for (size_t i = first; i < first + count; ++i) {
		const std::string& id = IdAt(i);
		// EVERY tile in view is on screen this frame, loaded or not, so the
		// eviction after this leaves it alone (code-review C111): the draw's mark
		// is a frame old by the time Evict looks.
		m_thumbs.Keep(id);
		if (loaded >= max) continue;
		auto* entry = m_thumbs.BeginLoad(id);
		if (!entry) continue;
		// Linear (LoadTextureThumb's default, as for everything the sprite batch
		// draws), as Game::SyncPortraits loads the full image, so a face looks
		// the same in the grid as in the party bar once picked.
		entry->data.texture = LoadTextureThumb(m_device, paths::Asset("portraits\\" + id), kThumbPx);
		++loaded;
	}
}

void PortraitPicker::SetFilter(int race, int sex, int age) {
	const auto valid = [](int v, size_t n) { return v >= 0 && v <= static_cast<int>(n); };
	if (valid(race, kRaces.size())) m_race = race;
	if (valid(sex, kSexes.size())) m_sex = sex;
	if (valid(age, kAges.size())) m_age = age;
	if (m_raceDrop) m_raceDrop->SetSelected(m_race);
	if (m_sexDrop) m_sexDrop->SetSelected(m_sex);
	if (m_ageDrop) m_ageDrop->SetSelected(m_age);
	ApplyFilter();
}

std::span<const char* const> PortraitPicker::Races() { return kRaces; }
std::span<const char* const> PortraitPicker::Sexes() { return kSexes; }
std::span<const char* const> PortraitPicker::Ages() { return kAges; }

void PortraitPicker::ScrollTo(float fraction) {
	m_scrollFraction = std::clamp(fraction, 0.0f, 1.0f);
}

PortraitPicker::Status PortraitPicker::GetStatus() const {
	Status s;
	s.open = m_open;
	s.shown = m_shown.size();
	s.total = m_entries.size();
	s.thumbs = m_thumbs.Size();
	s.race = m_race;
	s.sex = m_sex;
	s.age = m_age;
	s.counts = m_thumbs.Counts();
	if (m_grid) m_grid->Visible(s.firstVisible, s.visible);
	for (size_t i = s.firstVisible; i < s.firstVisible + s.visible; ++i) {
		const auto* entry = m_thumbs.Find(IdAt(i));
		if (!entry || !entry->tried) ++s.blank;
		else if (!entry->data.texture) ++s.missing;
	}
	return s;
}

void PortraitPicker::Update(const Input& input, float w, float h, float) {
	if (!m_open) return;
	m_thumbs.Tick();
	m_ui.UseFont(ui::FontRole::Body, std::clamp(h * 0.020f, 12.0f, 24.0f));
	m_ui.Update(input, w, h);
	if (!m_open) return; // a click picked, and closed us

	// Scrolls are applied AFTER the layout, when the grid has measured itself:
	// set before, the area clamps them against a height it does not know yet.
	if (m_grid && m_area && m_grid->Extent() > 0.0f) {
		const float view = m_area->ViewRect().h;
		const float range = std::max(m_grid->Extent() - view, 0.0f);
		if (m_scrollToCurrent) {
			m_scrollToCurrent = false;
			for (size_t i = 0; i < m_shown.size(); ++i)
				if (m_entries[m_shown[i]].id == m_current) {
					// Centred in the view where the range allows.
					const float y = m_grid->RowOffset(i) - (view - m_grid->Tile()) * 0.5f;
					m_area->SetScroll(std::clamp(y, 0.0f, range));
					break;
				}
		}
		if (m_scrollFraction >= 0.0f) {
			m_area->SetScroll(m_scrollFraction * range);
			m_scrollFraction = -1.0f;
		}
	}
	LoadVisibleThumbs(kThumbLoadsPerFrame);
	m_thumbs.Evict();
}

void PortraitPicker::Render(gfx::SpriteBatch& batch, float w, float h) {
	if (!m_open) return;
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.5f});
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	// An OPAQUE backing under the translucent panel face, as the item details
	// dialog has: over the sheet, two layers of content read as one.
	const ui::Theme& theme = m_ui.GetTheme();
	batch.DrawRect(panel, {theme.panel.x, theme.panel.y, theme.panel.z, 1.0f});
	ui::DrawPanelFace(m_ui, batch, panel);
	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
