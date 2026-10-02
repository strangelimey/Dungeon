// ============================================================================
// Game/PartyCreationPage.h - build the party before the game starts
// (docs/party-creation-plan.md, phase 3).
//
// Reached from Start New Game once the world is chosen, and the world is LOADED
// first (not its levels - Game::LoadWorld), so its races, starting items and
// skills are what the page offers and what Game::BuildMember checks against.
//
// The page EDITS SPECS, never Characters: a member is a party::MemberSpec (what
// was chosen), and what it comes to - stats, pools, kit - is a PREVIEW built
// through the same function the game will use (PartyCreationData::build), so
// the page cannot show one member and the game start another.
//
// Every edit goes through one of the public methods below, which the widgets
// AND the `partypage` dev command both call, so a scripted build exercises the
// page's own code. An edit that changes what the page SHOWS (a member added or
// selected, a race that turns a premade member into a made one) asks for a
// rebuild, taken by the owner at the top of the next frame - the cached widget
// pointers die with the tree, so it never happens inside a callback. Edits that
// only change numbers (a point spent, a skill picked, a name typed) leave the
// tree standing; Tick rewrites the live text each frame.
//
// The owner (GameUI) builds the stone card and hands Build its content area;
// the page owns the member faces it shows (a few textures, freed with a GPU
// drain - the SRV rule).
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Game/Character.h"
#include "Game/PartyRules.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::gfx {
class GraphicsDevice;
class Texture;
} // namespace dungeon::gfx

namespace dungeon::ui {
class Button;
class ColorPicker;
class DropDown;
class Label;
class TextField;
class Widget;
} // namespace dungeon::ui

namespace dungeon::game {

// What the world offers, gathered by Game when the page opens.
struct PartyRaceInfo {
	std::string id;          // races.cat id
	std::string name;        // localized
	std::string traits;      // localized: what the race does ("+2 Dexterity, ...")
	std::string portraitTag; // portraits.cat race tag the face picker filters by
	party::RaceStats stats;
};
struct PartyPick {
	std::string id;   // a skill or an item id
	std::string name; // localized
};
struct PartyCreationData {
	std::vector<PartyRaceInfo> races;
	std::vector<PartyPick> skills, items;
	// The default four as premade specs (their names, faces and Settings colours
	// filled in) - what the Default party button lays on the page.
	std::vector<party::MemberSpec> defaults;
	// The colour a member added in roster slot i starts with.
	std::array<Vec4, party::kMaxMembers> newColors{};
	// The member as the game will make it, with the game's pool rules applied
	// (Game::BuildMember); nullopt + why when it cannot be made.
	std::function<std::optional<Character>(const party::MemberSpec&, std::string&)> build;
};

class PartyCreationPage {
public:
	explicit PartyCreationPage(gfx::GraphicsDevice& device);
	~PartyCreationPage();

	// A fresh page: one new member, selected. Drops whatever was there.
	void Begin(PartyCreationData data);
	// Frees the faces (drains the GPU first). The page keeps its specs.
	void ReleaseFaces();

	// Builds the widgets into `area` (the card's content). The pointers it keeps
	// die with that tree; Build sets them again.
	void Build(ui::Widget& area);
	// True once after an edit that needs the tree rebuilt (see the header).
	bool TakeRebuild();
	// Before the tree updates: previews and faces for edited members, then the
	// live text and which buttons are usable.
	void Tick();

	// --- the edits: the widgets and `partypage` both call these ---------------
	void Select(size_t member);
	bool Add();                 // a new member at the end, selected
	bool Remove(size_t member); // never the last one
	void FillDefault();         // the default four, the first selected
	bool SetRace(size_t member, size_t race);
	bool Spend(size_t stat, int delta); // the selected member's free points
	void SetPick(bool skill, size_t slot, int choice); // -1 = none
	void SetName(std::string_view name);
	void SetColor(const Vec4& color);
	void SetPortrait(size_t member, const std::string& id);
	// One key=value word applied to the selected member, as `newparty` reads it
	// (party::ApplySpecField). False + why (English) when it cannot apply.
	bool SetField(std::string_view key, std::string_view value, std::string& why);

	// --- reading --------------------------------------------------------------
	size_t Count() const { return m_specs.size(); }
	size_t SelectedIndex() const { return m_selected; }
	const party::MemberSpec& Spec(size_t i) const { return m_specs[i]; }
	const std::vector<party::MemberSpec>& Specs() const { return m_specs; }
	// The built preview (nullopt when this spec cannot be made yet).
	const Character* Preview(size_t i) const;
	// Why Start would refuse, localized; empty = it may go.
	std::string Refusal() const;
	// The race the selected member is, as an index into the data (-1 = none).
	int RaceIndex(size_t member) const;
	const PartyCreationData& Data() const { return m_data; }
	// One line per member for the dev console (English).
	std::vector<std::string> StatusLines() const;

	// --- what the page asks of its owner ---------------------------------------
	std::function<void()> onStart;  // Start, once Refusal() is empty
	std::function<void()> onBack;   // Back
	std::function<void()> onClick;  // the button sound
	// The face picker for a member, filtered to their race's tag.
	std::function<void(size_t member, const std::string& raceTag)> onPickPortrait;

private:
	party::MemberSpec NewMember(size_t slot) const;
	void Touch() { m_previewDirty = true; }
	void Structural() { m_previewDirty = m_rebuild = true; }
	void RefreshPreviews();
	void RefreshFaces();
	const PartyRaceInfo* RaceOf(const party::MemberSpec& m) const;
	void BuildStrip(ui::Widget& row);
	void BuildIdentity(ui::Widget& col);
	void BuildStats(ui::Widget& col);
	void BuildPicks(ui::Widget& col);
	void BuildFooter(ui::Widget& row);

	gfx::GraphicsDevice& m_device;
	PartyCreationData m_data;
	std::vector<party::MemberSpec> m_specs;
	size_t m_selected = 0;
	bool m_rebuild = false;
	bool m_previewDirty = true;

	// Per member: the preview, and why it could not be built.
	std::vector<std::optional<Character>> m_previews;
	std::vector<std::string> m_previewWhy;
	// The faces, by member slot (the id each holds, so an unchanged one stays).
	std::array<std::unique_ptr<gfx::Texture>, party::kMaxMembers> m_faces;
	std::array<std::string, party::kMaxMembers> m_faceIds;

	// Widgets of the current tree (dead after a Clear; Build sets them again).
	ui::TextField* m_name = nullptr;
	ui::DropDown* m_race = nullptr;
	ui::Label* m_traits = nullptr;
	ui::Label* m_pools = nullptr;
	ui::ColorPicker* m_color = nullptr;
	ui::Label* m_points = nullptr;
	std::array<ui::Label*, party::kStats> m_statValue{};
	std::array<ui::Button*, party::kStats> m_minus{}, m_plus{};
	std::array<ui::DropDown*, party::kSkillPicks> m_skillDrop{};
	std::array<ui::DropDown*, party::kItemPicks> m_itemDrop{};
	ui::Label* m_refusal = nullptr;
	ui::Button* m_start = nullptr;
};

} // namespace dungeon::game
