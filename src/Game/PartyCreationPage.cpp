// ============================================================================
// Game/PartyCreationPage.cpp - the party creation page (see the header).
// ============================================================================
#include "Game/PartyCreationPage.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/PartyHudDraw.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "UI/Controls.h"
#include "UI/Font.h"
#include "UI/Layout.h"
#include "UI/Skin.h"
#include "UI/UIContext.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

namespace {

// The page's measures, in rem (the menu context's root font).
constexpr float kStripRem = 3.8f;  // the member slots
constexpr float kRowRem = 1.5f;    // a control row
constexpr float kLineRem = 1.25f;  // one line of text
constexpr float kLabelRem = 4.6f;  // the caption before a control
constexpr float kStatBtnRem = 1.6f; // a - / + stone
constexpr float kFooterRem = 1.9f;

// ONE MEMBER SLOT along the top: the face, the name and the race, the selected
// one sunk and outlined. Draws from the page every frame (the RosterMember
// idiom), so typing a name or picking a face shows at once with no rebuild. The
// slot after the last member is the Add slot; a slot past that is empty. A
// member's slot shows a remove mark in its corner while hovered, when the party
// has more than one member.
class MemberSlot : public ui::Widget {
public:
	MemberSlot(const gfx::Rect& rect, PartyCreationPage& page, size_t index)
		: m_page(page), m_index(index) {
		bounds = rect;
		debugName = "MemberSlot";
	}

	void UpdateSelf(ui::UIContext& ctx) override {
		m_hot = m_removeHot = false;
		const Input* input = ctx.CurrentInput();
		if (!input || ctx.IsMouseConsumed()) return;
		const float mx = input->MouseX(), my = input->MouseY();
		if (!Pixel().Contains(mx, my)) return;
		const size_t count = m_page.Count();
		const bool member = m_index < count;
		const bool add = m_index == count && count < party::kMaxMembers;
		if (!member && !add) return;
		ctx.ConsumeMouse();
		m_hot = true;
		m_removeHot = member && count > 1 && RemoveRect().Contains(mx, my);
		if (!input->WasMousePressed(MouseButton::Left)) return;
		if (m_page.onClick) m_page.onClick();
		if (m_removeHot) m_page.Remove(m_index);
		else if (member) m_page.Select(m_index);
		else m_page.Add();
	}

	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override {
		const size_t count = m_page.Count();
		if (m_index > count || m_index >= party::kMaxMembers) return; // nobody here
		const ui::Theme& theme = ctx.GetTheme();
		const ui::Skin* skin = ctx.GetSkin();
		const ui::Font& font = TextFont();
		const gfx::Rect r = Pixel();
		const bool member = m_index < count;
		const bool selected = member && m_index == m_page.SelectedIndex();

		// The face: a stone, sunk while selected; the Add slot is a well.
		const ui::Face face = !member ? ui::Face::Slot
								: selected ? ui::Face::ButtonDown
										   : ui::Face::Button;
		float in = Rem(0.25f);
		if (skin && skin->button.texture) {
			ui::DrawFace(batch, r, *skin, face, {1, 1, 1, 1});
			in = std::max(in, ui::FaceInset(*skin, face));
		} else {
			batch.DrawRect(r, m_hot ? theme.controlHot : theme.control);
			ui::DrawBorder(batch, r, theme.panelBorder);
		}

		if (!member) {
			const std::string_view add = loc::View("party.add");
			const float w = font.MeasureWidth(add);
			font.Draw(batch, add, r.x + (r.w - w) * 0.5f, r.y + (r.h - font.Height()) * 0.5f,
					  m_hot ? theme.text : theme.textDim);
			return;
		}

		// The portrait, square on the left (the tinted initial without one).
		const float side = r.h - 2 * in;
		const gfx::Rect pic{r.x + in, r.y + in, side, side};
		const party::MemberSpec& spec = m_page.Spec(m_index);
		if (const Character* c = m_page.Preview(m_index)) {
			DrawPortrait(batch, pic, *c, font, theme);
		} else {
			batch.DrawRect(pic, MutedIdentity({spec.color[0], spec.color[1], spec.color[2], 1}));
		}

		// Name over race, beside it.
		const float x = pic.x + pic.w + Rem(0.5f);
		const float room = r.x + r.w - in - Rem(1.2f) - x; // the remove mark's corner
		const float lineH = font.LineAdvance();
		const float top = r.y + (r.h - 2 * lineH) * 0.5f;
		const bool named = !spec.name.empty();
		ui::DrawFittedText(batch, font, named ? std::string_view(spec.name)
											  : loc::View("party.new_member"),
						   x, top, room, named ? theme.text : theme.textDim);
		const int race = m_page.RaceIndex(m_index);
		if (race >= 0)
			ui::DrawFittedText(batch, font, m_page.Data().races[static_cast<size_t>(race)].name,
							   x, top + lineH, room, theme.textDim);

		if (selected) ui::DrawBorder(batch, r, theme.accent);
		if (m_hot && count > 1) {
			const gfx::Rect x2 = RemoveRect();
			const std::string_view mark = "x";
			const float w = font.MeasureWidth(mark);
			font.Draw(batch, mark, x2.x + (x2.w - w) * 0.5f, x2.y + (x2.h - font.Height()) * 0.5f,
					  m_removeHot ? theme.accent : theme.textDim);
		}
	}

private:
	gfx::Rect RemoveRect() const {
		const gfx::Rect r = Pixel();
		const float s = Rem(1.2f);
		return {r.x + r.w - s - Rem(0.2f), r.y + Rem(0.2f), s, s};
	}

	PartyCreationPage& m_page;
	size_t m_index;
	bool m_hot = false;
	bool m_removeHot = false;
};

// A caption beside a control in one row.
ui::Stack* CaptionRow(ui::Widget& col, const std::string& caption) {
	auto* row = static_cast<ui::Stack&>(col).Row<ui::Stack>(ui::Len::Fixed(kRowRem), true);
	row->gapRem = 0.4f;
	row->Row<ui::Label>(ui::Len::Fixed(kLabelRem), caption)->centerV = true;
	return row;
}

} // namespace

PartyCreationPage::PartyCreationPage(gfx::GraphicsDevice& device) : m_device(device) {}

PartyCreationPage::~PartyCreationPage() { ReleaseFaces(); }

void PartyCreationPage::Begin(PartyCreationData data) {
	m_data = std::move(data);
	m_specs.clear();
	m_specs.push_back(NewMember(0));
	m_selected = 0;
	Structural();
}

void PartyCreationPage::ReleaseFaces() {
	bool any = false;
	for (const auto& f : m_faces) any = any || f;
	if (any) m_device.WaitIdle(); // in-flight frames still sample them
	for (auto& f : m_faces) f.reset();
	for (auto& id : m_faceIds) id.clear();
	for (auto& p : m_previews)
		if (p) p->portrait = nullptr;
}

party::MemberSpec PartyCreationPage::NewMember(size_t slot) const {
	party::MemberSpec m;
	// Human when the world has humans (the plain choice), else its first race.
	for (const PartyRaceInfo& r : m_data.races)
		if (r.id == "human") m.race = r.id;
	if (m.race.empty() && !m_data.races.empty()) m.race = m_data.races.front().id;
	const Vec4 c = m_data.newColors[std::min(slot, party::kMaxMembers - 1)];
	m.color = {c.x, c.y, c.z, 1.0f};
	m.colorSet = true;
	return m;
}

// --- edits ---------------------------------------------------------------------

void PartyCreationPage::Select(size_t member) {
	if (member >= m_specs.size() || member == m_selected) return;
	m_selected = member;
	Structural();
}

bool PartyCreationPage::Add() {
	if (m_specs.size() >= party::kMaxMembers) return false;
	m_specs.push_back(NewMember(m_specs.size()));
	m_selected = m_specs.size() - 1;
	Structural();
	return true;
}

bool PartyCreationPage::Remove(size_t member) {
	if (member >= m_specs.size() || m_specs.size() <= party::kMinMembers) return false;
	m_specs.erase(m_specs.begin() + static_cast<std::ptrdiff_t>(member));
	if (m_selected >= m_specs.size() || m_selected > member) m_selected = m_selected ? m_selected - 1 : 0;
	Structural();
	return true;
}

void PartyCreationPage::FillDefault() {
	if (m_data.defaults.empty()) return;
	m_specs = m_data.defaults;
	m_selected = 0;
	Structural();
}

bool PartyCreationPage::SetRace(size_t member, size_t race) {
	if (member >= m_specs.size() || race >= m_data.races.size()) return false;
	party::MemberSpec& m = m_specs[member];
	if (m.premade >= 0) {
		// A premade member given another race becomes an ordinary made one: the
		// race's base and free points, no kit (the plan). Who they are stays.
		if (const Character* c = Preview(member)) {
			if (m.name.empty()) m.name = c->name;
			if (m.portrait.empty()) m.portrait = c->portraitId;
			if (!m.colorSet)
				m.color = {c->portraitColor.x, c->portraitColor.y, c->portraitColor.z, 1.0f};
		}
		m.colorSet = true;
		m.premade = -1;
		m.spent = {};
		m.skills.clear();
		m.items.clear();
		m.race = m_data.races[race].id;
		Structural(); // the picks column appears
		return true;
	}
	m.race = m_data.races[race].id;
	// A smaller budget hands back what no longer fits, from the last stat down.
	const party::RaceStats& rs = m_data.races[race].stats;
	for (size_t s = party::kStats; s-- > 0;)
		while (m.spent[s] > 0 && party::PointsSpent(m.spent) > party::PointBudget(rs)) --m.spent[s];
	Touch();
	return true;
}

bool PartyCreationPage::Spend(size_t stat, int delta) {
	if (m_selected >= m_specs.size() || stat >= party::kStats) return false;
	party::MemberSpec& m = m_specs[m_selected];
	const PartyRaceInfo* race = RaceOf(m);
	if (m.premade >= 0 || !race) return false;
	if (delta > 0 && !party::CanSpend(race->stats, m.spent)) return false;
	if (delta < 0 && !party::CanRefund(m.spent, stat)) return false;
	m.spent[stat] += delta > 0 ? 1 : -1;
	Touch();
	return true;
}

void PartyCreationPage::SetPick(bool skill, size_t slot, int choice) {
	if (m_selected >= m_specs.size()) return;
	party::MemberSpec& m = m_specs[m_selected];
	if (m.premade >= 0) return;
	const std::vector<PartyPick>& from = skill ? m_data.skills : m_data.items;
	std::vector<std::string>& picks = skill ? m.skills : m.items;
	const size_t picksMax = skill ? party::kSkillPicks : party::kItemPicks;
	if (slot >= picksMax) return;
	// Picks are kept by SLOT (a two-entry list with "" for an empty slot) while
	// editing, then compacted: what BuildMember and a save see has no gaps.
	std::array<std::string, 2> slots{};
	for (size_t i = 0; i < picks.size() && i < slots.size(); ++i) slots[i] = picks[i];
	slots[slot] = choice >= 0 && static_cast<size_t>(choice) < from.size()
					  ? from[static_cast<size_t>(choice)].id
					  : std::string();
	// The same skill twice is one skill: the other slot lets go of it.
	if (skill && !slots[slot].empty() && slots[1 - slot] == slots[slot]) slots[1 - slot].clear();
	picks.clear();
	for (const std::string& s : slots)
		if (!s.empty()) picks.push_back(s);
	Touch();
}

void PartyCreationPage::SetName(std::string_view name) {
	if (m_selected >= m_specs.size()) return;
	m_specs[m_selected].name = std::string(name.substr(0, party::kMaxNameLength));
	Touch();
}

void PartyCreationPage::SetColor(const Vec4& color) {
	if (m_selected >= m_specs.size()) return;
	party::MemberSpec& m = m_specs[m_selected];
	m.color = {color.x, color.y, color.z, 1.0f};
	m.colorSet = true;
	Touch();
}

void PartyCreationPage::SetPortrait(size_t member, const std::string& id) {
	if (member >= m_specs.size()) return;
	m_specs[member].portrait = id;
	Touch();
}

bool PartyCreationPage::SetField(std::string_view key, std::string_view value, std::string& why) {
	if (m_selected >= m_specs.size()) {
		why = "no member is selected";
		return false;
	}
	// Race is the page's own edit (it turns a premade member into a made one).
	if (key == "race") {
		for (size_t r = 0; r < m_data.races.size(); ++r)
			if (m_data.races[r].id == value) return SetRace(m_selected, r);
		why = "no race '" + std::string(value) + "'";
		return false;
	}
	if (!party::ApplySpecField(m_specs[m_selected], key, value, why)) return false;
	// Anything that changes which rows show (premade=) rebuilds; the rest is text.
	if (key == "premade") Structural();
	else Touch();
	return true;
}

// --- reading -------------------------------------------------------------------

const Character* PartyCreationPage::Preview(size_t i) const {
	return i < m_previews.size() && m_previews[i] ? &*m_previews[i] : nullptr;
}

const PartyRaceInfo* PartyCreationPage::RaceOf(const party::MemberSpec& m) const {
	for (const PartyRaceInfo& r : m_data.races)
		if (r.id == m.race) return &r;
	return nullptr;
}

int PartyCreationPage::RaceIndex(size_t member) const {
	if (member >= m_specs.size()) return -1;
	// A premade member's race is their character's (the spec names none).
	std::string_view id = m_specs[member].race;
	if (m_specs[member].premade >= 0)
		if (const Character* c = Preview(member)) id = c->raceId;
	for (size_t r = 0; r < m_data.races.size(); ++r)
		if (m_data.races[r].id == id) return static_cast<int>(r);
	return -1;
}

std::string PartyCreationPage::Refusal() const {
	for (size_t i = 0; i < m_specs.size(); ++i) {
		const party::MemberSpec& m = m_specs[i];
		const Character* c = Preview(i);
		// A premade member with no name of their own given keeps theirs.
		const bool keepsName = m.premade >= 0 && m.name.empty();
		if (!keepsName && !party::NameValid(m.name))
			return loc::Format("party.why.name", i + 1);
		const std::string who = keepsName && c ? c->name : m.name;
		if (m.portrait.empty() && !(m.premade >= 0 && c && !c->portraitId.empty()))
			return loc::Format("party.why.face", who);
		if (!c) return loc::Format("party.why.build", who, i < m_previewWhy.size() ? m_previewWhy[i] : "");
	}
	return {};
}

std::vector<std::string> PartyCreationPage::StatusLines() const {
	std::vector<std::string> out;
	for (size_t i = 0; i < m_specs.size(); ++i) {
		const party::MemberSpec& m = m_specs[i];
		const Character* c = Preview(i);
		std::string stats = "-", items;
		if (c) {
			stats.clear();
			for (size_t s = 0; s < party::kStats; ++s)
				stats += std::format("{}{}", s ? "," : "", c->*(kStats[s].value));
		}
		for (const std::string& it : m.items) items += (items.empty() ? "" : ",") + it;
		std::string skills;
		for (const std::string& sk : m.skills) skills += (skills.empty() ? "" : ",") + sk;
		const PartyRaceInfo* race = RaceOf(m);
		// A premade member spends nothing: their stats are authored.
		const std::string points =
			m.premade < 0 && race
				? std::format("{}/{}", party::PointsSpent(m.spent), party::PointBudget(race->stats))
				: std::string("-");
		out.push_back(std::format(
			"member {}{} name='{}' race={} face={} premade={} points={} stats={} skills={} "
			"items={}",
			i, i == m_selected ? "*" : "", c && m.name.empty() ? c->name : m.name,
			RaceIndex(i) >= 0 ? m_data.races[static_cast<size_t>(RaceIndex(i))].id : "-",
			m.portrait.empty() && c ? c->portraitId : m.portrait, m.premade, points, stats,
			skills.empty() ? "-" : skills, items.empty() ? "-" : items));
	}
	const std::string why = Refusal();
	out.push_back(why.empty() ? std::string("start: ready") : "start: refused - " + why);
	return out;
}

// --- per frame -----------------------------------------------------------------

bool PartyCreationPage::TakeRebuild() { return std::exchange(m_rebuild, false); }

void PartyCreationPage::RefreshPreviews() {
	m_previewDirty = false;
	m_previews.assign(m_specs.size(), std::nullopt);
	m_previewWhy.assign(m_specs.size(), {});
	if (!m_data.build) return;
	for (size_t i = 0; i < m_specs.size(); ++i) {
		// The preview is about the NUMBERS: a member not yet named is still
		// shown, under a stand-in the Start check refuses separately.
		party::MemberSpec spec = m_specs[i];
		const bool keepsName = spec.premade >= 0 && spec.name.empty();
		if (!keepsName && !party::NameValid(spec.name)) spec.name = "?";
		m_previews[i] = m_data.build(spec, m_previewWhy[i]);
	}
}

void PartyCreationPage::RefreshFaces() {
	bool drained = false;
	for (size_t i = 0; i < party::kMaxMembers; ++i) {
		std::string want;
		if (i < m_specs.size()) {
			want = m_specs[i].portrait;
			if (want.empty())
				if (const Character* c = Preview(i)) want = c->portraitId;
		}
		if (want != m_faceIds[i]) {
			if (m_faces[i] && !drained) {
				m_device.WaitIdle();
				drained = true;
			}
			m_faces[i].reset();
			if (!want.empty())
				m_faces[i] = TryLoadTextureFile(m_device, paths::Asset("portraits\\" + want));
			m_faceIds[i] = want;
		}
		if (i < m_previews.size() && m_previews[i]) m_previews[i]->portrait = m_faces[i].get();
	}
}

void PartyCreationPage::Tick() {
	if (m_previewDirty) RefreshPreviews();
	RefreshFaces();
	if (m_selected >= m_specs.size()) return;
	const party::MemberSpec& m = m_specs[m_selected];
	const Character* c = Preview(m_selected);
	const PartyRaceInfo* race = RaceOf(m);
	const int raceIndex = RaceIndex(m_selected);

	// What the race gives, after its name - not for a premade member, whose
	// numbers are their own (a human's "+2 free points" would be a promise they
	// do not get).
	if (m_traits) {
		const PartyRaceInfo* shown =
			raceIndex >= 0 ? &m_data.races[static_cast<size_t>(raceIndex)] : nullptr;
		m_traits->text = shown && m.premade < 0 && !shown->traits.empty()
							 ? shown->name + ": " + shown->traits
							 : std::string();
	}
	if (m_pools)
		m_pools->text = c ? loc::Format("party.pools", static_cast<int>(c->maxHealth + 0.5f),
										static_cast<int>(c->maxStamina + 0.5f),
										static_cast<int>(c->maxMana + 0.5f))
						  : std::string();
	const bool made = m.premade < 0 && race;
	if (m_points)
		m_points->text = made ? loc::Format("party.points",
											party::PointBudget(race->stats) -
												party::PointsSpent(m.spent),
											party::PointBudget(race->stats))
							  : loc::Tr("party.premade_stats");
	for (size_t s = 0; s < party::kStats; ++s) {
		if (m_statValue[s])
			m_statValue[s]->text = c ? std::to_string(c->*(kStats[s].value)) : std::string("-");
		if (m_minus[s]) m_minus[s]->enabled = made && party::CanRefund(m.spent, s);
		if (m_plus[s]) m_plus[s]->enabled = made && party::CanSpend(race->stats, m.spent);
	}
	const std::string why = Refusal();
	if (m_refusal) m_refusal->text = why;
	if (m_start) m_start->enabled = why.empty();
}

// --- the tree ------------------------------------------------------------------

void PartyCreationPage::Build(ui::Widget& area) {
	m_name = nullptr;
	m_race = nullptr;
	m_traits = m_pools = m_points = m_refusal = nullptr;
	m_color = nullptr;
	m_statValue = {};
	m_minus = m_plus = {};
	m_skillDrop = {};
	m_itemDrop = {};
	m_start = nullptr;
	if (m_previewDirty) RefreshPreviews(); // the race/premade rows read them

	auto* col = area.Add<ui::Stack>(gfx::Rect{0, 0, 1, 1});
	col->gapRem = 0.4f;
	BuildStrip(*col->Row<ui::Stack>(ui::Len::Fixed(kStripRem), true));
	col->Row<ui::Separator>(ui::Len::Fixed(0.3f));
	// What the selected member's race gives, across the whole card: a race's
	// line is longer than any one column.
	m_traits = col->Row<ui::Label>(ui::Len::Fixed(kLineRem), std::string());
	m_traits->dim = true;
	auto* editor = col->Row<ui::Stack>(ui::Len::Fill(), true);
	editor->gapRem = 1.2f;
	BuildIdentity(*editor->Row<ui::Stack>(ui::Len::Fill(1.15f)));
	BuildStats(*editor->Row<ui::Stack>(ui::Len::Fill(0.85f)));
	BuildPicks(*editor->Row<ui::Stack>(ui::Len::Fill(1.0f)));
	m_refusal = col->Row<ui::Label>(ui::Len::Fixed(kLineRem), std::string());
	m_refusal->accent = true;
	BuildFooter(*col->Row<ui::Stack>(ui::Len::Fixed(kFooterRem), true));
}

void PartyCreationPage::BuildStrip(ui::Widget& w) {
	auto& row = static_cast<ui::Stack&>(w);
	row.gapRem = 0.5f;
	for (size_t i = 0; i < party::kMaxMembers; ++i)
		row.Row<MemberSlot>(ui::Len::Fill(), *this, i);
}

void PartyCreationPage::BuildIdentity(ui::Widget& w) {
	auto& col = static_cast<ui::Stack&>(w);
	col.gapRem = 0.3f;
	if (m_selected >= m_specs.size()) return;
	const party::MemberSpec& m = m_specs[m_selected];
	const Character* c = Preview(m_selected);

	m_name = CaptionRow(col, loc::Tr("party.name"))
				 ->Row<ui::TextField>(ui::Len::Fill(), m.name.empty() && c && m.premade >= 0
															  ? c->name
															  : m.name);
	m_name->placeholder = loc::Tr("party.name_placeholder");
	m_name->maxLength = party::kMaxNameLength;
	m_name->onChange = [this] {
		if (m_name) SetName(m_name->text);
	};

	std::vector<std::string> races;
	for (const PartyRaceInfo& r : m_data.races) races.push_back(r.name);
	m_race = CaptionRow(col, loc::Tr("party.race"))
				 ->Row<ui::DropDown>(ui::Len::Fill(), std::move(races),
									 std::max(0, RaceIndex(m_selected)), [this](int i) {
										 if (i >= 0) SetRace(m_selected, static_cast<size_t>(i));
									 });

	CaptionRow(col, loc::Tr("party.face"))
		->Row<ui::Button>(ui::Len::Fill(), loc::Tr("party.face_choose"), [this] {
			if (onClick) onClick();
			const int r = RaceIndex(m_selected);
			if (onPickPortrait)
				onPickPortrait(m_selected,
							   r >= 0 ? m_data.races[static_cast<size_t>(r)].portraitTag : "");
		});

	const Vec4 color = m.premade >= 0 && !m.colorSet && c
						   ? c->portraitColor
						   : Vec4{m.color[0], m.color[1], m.color[2], 1.0f};
	m_color = col.Row<ui::ColorPicker>(ui::Len::Fixed(kRowRem), loc::Tr("party.color"), color,
									   [this](const Vec4& v) { SetColor(v); });

	m_pools = col.Row<ui::Label>(ui::Len::Fixed(kLineRem), std::string());
	m_pools->dim = true;
}

void PartyCreationPage::BuildStats(ui::Widget& w) {
	auto& col = static_cast<ui::Stack&>(w);
	col.gapRem = 0.2f;
	m_points = col.Row<ui::Label>(ui::Len::Fixed(kLineRem), std::string());
	m_points->accent = true;
	// A premade member's stats are authored: shown, with no stones to move them.
	const bool made = m_selected < m_specs.size() && m_specs[m_selected].premade < 0;
	for (size_t s = 0; s < party::kStats; ++s) {
		auto* row = col.Row<ui::Stack>(ui::Len::Fixed(kRowRem - 0.1f), true);
		row->gapRem = 0.3f;
		row->Row<ui::Label>(ui::Len::Fill(), loc::Tr(std::format("attr.{}", kStats[s].id)))
			->centerV = true;
		m_statValue[s] = row->Row<ui::Label>(ui::Len::Fixed(1.6f), std::string());
		m_statValue[s]->centerV = true;
		if (!made) {
			row->Space(ui::Len::Fixed(2 * kStatBtnRem + 0.3f));
			continue;
		}
		m_minus[s] = row->Row<ui::Button>(ui::Len::Fixed(kStatBtnRem), "-", [this, s] {
			if (onClick) onClick();
			Spend(s, -1);
		});
		m_plus[s] = row->Row<ui::Button>(ui::Len::Fixed(kStatBtnRem), "+", [this, s] {
			if (onClick) onClick();
			Spend(s, +1);
		});
	}
}

void PartyCreationPage::BuildPicks(ui::Widget& w) {
	auto& col = static_cast<ui::Stack&>(w);
	col.gapRem = 0.2f;
	if (m_selected >= m_specs.size()) return;
	const party::MemberSpec& m = m_specs[m_selected];
	if (m.premade >= 0) {
		// The default four keep what they were authored with.
		col.Row<ui::Label>(ui::Len::Fixed(kLineRem), loc::Tr("party.premade"))->dim = true;
		col.Row<ui::Label>(ui::Len::Fixed(kLineRem), loc::Tr("party.premade_race"))->dim = true;
		return;
	}
	const auto drops = [&](bool skill, auto& out, const char* headKey) {
		col.Row<ui::Label>(ui::Len::Fixed(kLineRem),
						   skill ? loc::Format(headKey, party::kSkillBoostLevel)
								 : loc::Tr(headKey));
		const std::vector<PartyPick>& from = skill ? m_data.skills : m_data.items;
		const std::vector<std::string>& picks = skill ? m.skills : m.items;
		for (size_t slot = 0; slot < out.size(); ++slot) {
			std::vector<std::string> names{loc::Tr("party.none")};
			int selected = 0;
			for (size_t i = 0; i < from.size(); ++i) {
				names.push_back(from[i].name);
				if (slot < picks.size() && picks[slot] == from[i].id)
					selected = static_cast<int>(i) + 1;
			}
			out[slot] = col.Row<ui::DropDown>(ui::Len::Fixed(kRowRem), std::move(names), selected,
											  [this, skill, slot](int i) { SetPick(skill, slot, i - 1); });
		}
	};
	drops(true, m_skillDrop, "party.skills");
	col.Space(ui::Len::Fixed(0.2f));
	drops(false, m_itemDrop, "party.items");
}

void PartyCreationPage::BuildFooter(ui::Widget& w) {
	auto& row = static_cast<ui::Stack&>(w);
	row.gapRem = 0.6f;
	row.Row<ui::Button>(ui::Len::Fixed(8.5f), loc::Tr("party.default"), [this] {
		if (onClick) onClick();
		FillDefault();
	})->carved = true;
	row.Space(ui::Len::Fill());
	row.Row<ui::Button>(ui::Len::Fixed(7.0f), loc::Tr("menu.back"), [this] {
		if (onClick) onClick();
		if (onBack) onBack();
	})->carved = true;
	m_start = row.Row<ui::Button>(ui::Len::Fixed(7.0f), loc::Tr("party.start"), [this] {
		if (!Refusal().empty()) return;
		if (onStart) onStart();
	});
	m_start->carved = true;
}

} // namespace dungeon::game
