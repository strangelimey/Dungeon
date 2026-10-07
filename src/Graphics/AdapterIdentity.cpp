// ============================================================================
// Graphics/AdapterIdentity.cpp - see AdapterIdentity.h.
// ============================================================================
#include "Graphics/AdapterIdentity.h"

#include <charconv>
#include <format>

namespace dungeon::gfx {

std::string EncodeAdapterIdentity(const AdapterIdentity& id) {
	if (id.Empty()) return {};
	std::string text = std::format("{:x}:{:x}:{:x}:{:x}", id.vendorId, id.deviceId,
								   id.subSysId, id.revision);
	if (!id.description.empty()) {
		text += ' ';
		text += id.description;
	}
	return text;
}

bool DecodeAdapterIdentity(std::string_view text, AdapterIdentity& out) {
	// The ids run to the first space; the description is everything after it.
	const size_t space = text.find(' ');
	const std::string_view ids = text.substr(0, space);
	u32 parsed[4] = {};
	size_t at = 0;
	for (int i = 0; i < 4; ++i) {
		const size_t end = i < 3 ? ids.find(':', at) : ids.size();
		if (end == std::string_view::npos || end == at) return false;
		const auto r = std::from_chars(ids.data() + at, ids.data() + end, parsed[i], 16);
		if (r.ec != std::errc{} || r.ptr != ids.data() + end) return false;
		at = end + 1;
	}
	AdapterIdentity id;
	id.vendorId = parsed[0];
	id.deviceId = parsed[1];
	id.subSysId = parsed[2];
	id.revision = parsed[3];
	if (space != std::string_view::npos) {
		std::string_view desc = text.substr(space + 1);
		while (!desc.empty() && (desc.back() == ' ' || desc.back() == '\r')) desc.remove_suffix(1);
		id.description = std::string(desc);
	}
	out = std::move(id);
	return true;
}

int ResolveAdapterIdentity(const AdapterIdentity& saved,
						   std::span<const AdapterIdentity> installed) {
	if (saved.Empty()) return -1;
	// The tier a candidate reaches (higher = closer), 0 = no match at all.
	const auto tier = [&](const AdapterIdentity& a) -> int {
		if (a.vendorId != saved.vendorId || a.deviceId != saved.deviceId) return 0;
		if (a.subSysId != saved.subSysId) return 1;
		if (a.revision != saved.revision) return 2;
		return a.description == saved.description ? 4 : 3;
	};
	int best = -1, bestTier = 0;
	for (size_t i = 0; i < installed.size(); ++i) {
		const int t = tier(installed[i]);
		if (t > bestTier) { // strictly: a tie keeps the first
			bestTier = t;
			best = static_cast<int>(i);
		}
	}
	return best;
}

} // namespace dungeon::gfx
