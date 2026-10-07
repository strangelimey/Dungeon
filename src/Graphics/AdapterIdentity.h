// ============================================================================
// Graphics/AdapterIdentity.h - which GPU a saved choice names, across reboots
// (code-review C197).
//
// The Video tab's adapter used to be saved as its packed LUID. A LUID is only
// unique until the machine restarts, and usually changes on a reboot, so the
// saved GPU quietly stopped matching anything and the device fell back to the
// high-performance one. What IS stable is what the hardware says it is: the PCI
// vendor, device, subsystem and revision ids, plus the driver's description.
// That is what settings.ini keeps now (`adapter_id=`), and the boot resolves it
// to whichever LUID that adapter has THIS run.
//
// PURE - no DXGI, nothing but the standard library - so RollTest links it and
// checks the resolution rule (the Defense.h bargain). DisplayEnum fills an
// identity from DXGI_ADAPTER_DESC1; this file only compares them.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <span>
#include <string>
#include <string_view>

namespace dungeon::gfx {

// What an adapter says it is. All zero with no description = none (auto).
struct AdapterIdentity {
	u32 vendorId = 0;
	u32 deviceId = 0;
	u32 subSysId = 0;
	u32 revision = 0;
	std::string description; // DXGI_ADAPTER_DESC1::Description, UTF-8

	bool Empty() const {
		return vendorId == 0 && deviceId == 0 && subSysId == 0 && revision == 0 &&
			   description.empty();
	}
};

// The settings.ini spelling: four hex ids joined by ':' then one space and the
// description, which may hold spaces and colons of its own -
// "10de:2684:16f310de:a1 NVIDIA GeForce RTX 4090". An empty identity is "".
std::string EncodeAdapterIdentity(const AdapterIdentity& id);

// Reads that spelling back; false (and `out` untouched) for anything else -
// fewer than four ids, a non-hex digit, an id past 32 bits.
bool DecodeAdapterIdentity(std::string_view text, AdapterIdentity& out);

// The installed adapter a saved identity names, as an index into `installed`;
// -1 for none (an empty identity, or that GPU is gone). The closest match wins:
//   1. every id AND the description;
//   2. every id - a driver update can reword the description;
//   3. vendor, device and subsystem - the same board at another revision;
//   4. vendor and device - the same chip on another board.
// A tie goes to the FIRST in the list, which is what makes two identical cards
// resolve the same way every run (they are told apart only by their LUIDs,
// which do not survive a reboot - there is nothing stable left to tell them by).
int ResolveAdapterIdentity(const AdapterIdentity& saved,
						   std::span<const AdapterIdentity> installed);

} // namespace dungeon::gfx
