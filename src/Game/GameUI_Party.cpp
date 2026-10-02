// ============================================================================
// Game/GameUI_Party.cpp - the party creation page in the menus
// (docs/party-creation-plan.md, phase 3). The page itself is PartyCreationPage;
// this is where it is opened, built into the menu's page context, left, and
// started, and where the face picker is put over it.
// ============================================================================
#include "Game/GameUI.h"

#include "Core/Log.h"
#include "Game/MenuPanel.h"

#include <algorithm>

namespace dungeon::game {

namespace {
constexpr float kPartyCardW = 0.86f;   // wider than the lists: three columns
constexpr float kPartyCardTop = 0.05f; // and taller: no big title above it
} // namespace

void GameUI::OpenPartyPage(PartyCreationData data) {
	m_partyPage->onClick = [this] { Click(); };
	m_partyPage->onBack = [this] { LeavePartyPage(); };
	m_partyPage->onStart = [this] {
		std::string why;
		StartPartyPage(why);
	};
	m_partyPage->onPickPortrait = [this](size_t member, const std::string& tag) {
		OpenPartyPortraitPicker(member, tag);
	};
	m_partyPage->Begin(std::move(data));
	// Built at the top of the next frame: this may run inside the world list's
	// row callback, in the very context the page is built into.
	m_partyBuildPending = true;
	m_partyLeavePending = false;
}

void GameUI::BuildPartyPage() {
	m_savesUi.Clear();
	m_saveField = nullptr; // the Save page's pointers die with this context too
	m_saveButton = nullptr;
	PageCard* card = AddPageCard(kPartyCardW, "party.title", kPartyCardTop);
	m_partyPage->Build(*card);
	m_partyPage->Tick(); // the live text, before the first draw
	m_menuPage = MenuPage::Party;
}

void GameUI::RefreshPartyPageIfDirty() {
	if (m_partyLeavePending) {
		m_partyLeavePending = false;
		m_partyBuildPending = false;
		if (PortraitPickerOpen()) ClosePortraitPicker();
		m_partyPage->ReleaseFaces();
		if (m_partyFromWorlds) {
			OpenWorldsPage();
		} else {
			m_savesUi.Clear();
			m_menuPage = MenuPage::Main;
		}
		return;
	}
	// A new tree when the page was opened, or an edit changed what it shows.
	if (m_partyBuildPending || (PartyPageOpen() && m_partyPage->TakeRebuild())) {
		m_partyBuildPending = false;
		m_partyPage->TakeRebuild();
		BuildPartyPage();
	}
}

void GameUI::LeavePartyPage() {
	if (PartyPageOpen() || m_partyBuildPending) m_partyLeavePending = true;
}

bool GameUI::StartPartyPage(std::string& why) {
	if (!PartyPageActive()) {
		why = "the party page is not open";
		return false;
	}
	why = m_partyPage->Refusal();
	if (!why.empty()) return false;
	if (!onStartParty) {
		why = "nothing is wired to start a party";
		return false;
	}
	if (PortraitPickerOpen()) ClosePortraitPicker();
	Click(0.6f);
	// The page's tree stays until the next page is built (this may be its own
	// button's callback); only the faces go now, and the menu leaves the page.
	m_partyPage->ReleaseFaces();
	m_partyBuildPending = false;
	m_menuPage = MenuPage::Main;
	return onStartParty(m_partyPage->Specs(), why);
}

// --- Settings -> UI -> Party Colors (phase 4) ------------------------------------

bool GameUI::MemberColorInPlay(size_t slot) const {
	return slot < m_characters.size() && partyInPlay && partyInPlay();
}

// Each row names whoever holds its slot and shows THEIR colour - a made member's
// is their own, not the slot default - or, with no party in play (the title) or
// nobody in the slot, "Member n" and the default a new member there starts with.
// Run when the page opens or is rebuilt: the party may have changed since.
void GameUI::SyncMemberColorPickers() {
	for (size_t i = 0; i < m_memberColorPickers.size(); ++i) {
		ui::ColorPicker* picker = m_memberColorPickers[i];
		if (!picker) continue;
		if (MemberColorInPlay(i)) {
			picker->label = m_characters[i].name;
			picker->SetColor(m_characters[i].portraitColor);
		} else {
			picker->label = loc::Format("settings.member_n", i + 1);
			picker->SetColor(m_settings.memberColors[i]);
		}
	}
}

void GameUI::OpenPartyPortraitPicker(size_t member, const std::string& raceTag) {
	if (!m_portraitPicker || !m_partyPage || member >= m_partyPage->Count()) return;
	const party::MemberSpec& spec = m_partyPage->Spec(member);
	const Character* c = m_partyPage->Preview(member);
	const std::string name = !spec.name.empty() ? spec.name
							 : c && spec.premade >= 0 ? c->name
													  : loc::Tr("party.new_member");
	const std::string current = !spec.portrait.empty() ? spec.portrait
								: c ? c->portraitId
									: std::string();
	m_portraitPicker->Open(loc::FormatLine("portrait.pick.title", name).View(), current,
						   [this, member](const std::string& id) {
							   if (m_partyPage) m_partyPage->SetPortrait(member, id);
						   });
	// Filtered to the member's race (the picker's own drop-downs can widen it).
	const auto races = PortraitPicker::Races();
	int race = 0;
	for (size_t i = 0; i < races.size(); ++i)
		if (raceTag == races[i]) race = static_cast<int>(i) + 1;
	m_portraitPicker->SetFilter(race, 0, 0);
	// tools\InGameTest.ps1 reads this to know its sweep audited the picker.
	log::Info("portrait picker: open for {} (party creation)", name);
}

} // namespace dungeon::game
