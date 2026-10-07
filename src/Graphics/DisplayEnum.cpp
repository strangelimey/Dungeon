// ============================================================================
// Graphics/DisplayEnum.cpp - see DisplayEnum.h.
// ============================================================================
#include "Graphics/DisplayEnum.h"

#include "Core/Log.h"
#include "Core/StringUtil.h"
#include "Graphics/GraphicsDevice.h" // kBackBufferFormat
#include "Graphics/D3DUtil.h"        // ComPtr

#include <Windows.h> // GetMonitorInfoW, EnumDisplayMonitors, EnumDisplaySettingsW
#include <dxgi1_6.h>

#include <algorithm>
#include <format>

namespace dungeon::gfx {

namespace {

AdapterIdentity IdentityOf(const DXGI_ADAPTER_DESC1& desc) {
	AdapterIdentity id;
	id.vendorId = desc.VendorId;
	id.deviceId = desc.DeviceId;
	id.subSysId = desc.SubSysId;
	id.revision = desc.Revision;
	id.description = str::Narrow(desc.Description);
	return id;
}

void AddMode(OutputInfo& out, u32 w, u32 h) {
	if (w == 0 || h == 0) return;
	const bool seen = std::any_of(out.modes.begin(), out.modes.end(), [&](const DisplayMode& d) {
		return d.width == w && d.height == h;
	});
	if (!seen) out.modes.push_back({w, h});
}

// Largest first; the current desktop size when the driver listed nothing.
void FinishModes(OutputInfo& out) {
	std::sort(out.modes.begin(), out.modes.end(), [](const DisplayMode& a, const DisplayMode& b) {
		return u64{a.width} * a.height > u64{b.width} * b.height;
	});
	if (out.modes.empty() && out.width > 0 && out.height > 0)
		out.modes.push_back({static_cast<u32>(out.width), static_cast<u32>(out.height)});
}

// The desktop rect, work area, primary flag and device name, from the monitor
// itself. False when it will not answer.
bool ReadMonitor(HMONITOR hmon, OutputInfo& out) {
	MONITORINFOEXW mi{};
	mi.cbSize = sizeof(mi);
	if (!hmon || !GetMonitorInfoW(hmon, &mi)) return false;
	out.monitor = hmon;
	out.x = mi.rcMonitor.left;
	out.y = mi.rcMonitor.top;
	out.width = mi.rcMonitor.right - mi.rcMonitor.left;
	out.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
	out.workX = mi.rcWork.left;
	out.workY = mi.rcWork.top;
	out.workWidth = mi.rcWork.right - mi.rcWork.left;
	out.workHeight = mi.rcWork.bottom - mi.rcWork.top;
	out.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
	if (out.device.empty()) out.device = str::Narrow(mi.szDevice);
	return true;
}

// The modes GDI lists for a monitor no DXGI adapter reported (32 bpp ones).
void ReadGdiModes(const std::wstring& device, OutputInfo& out) {
	DEVMODEW dm{};
	dm.dmSize = sizeof(dm);
	for (DWORD i = 0; EnumDisplaySettingsW(device.c_str(), i, &dm); ++i)
		if (dm.dmBitsPerPel == 32) AddMode(out, dm.dmPelsWidth, dm.dmPelsHeight);
}

BOOL CALLBACK CollectMonitor(HMONITOR hmon, HDC, LPRECT, LPARAM user) {
	reinterpret_cast<std::vector<HMONITOR>*>(user)->push_back(hmon);
	return TRUE;
}

} // namespace

u64 PackLuid(i32 highPart, u32 lowPart) {
	return (static_cast<u64>(static_cast<u32>(highPart)) << 32) | lowPart;
}

int DisplayList::MonitorIndex(std::string_view device) const {
	if (device.empty()) return -1;
	for (size_t i = 0; i < monitors.size(); ++i)
		if (monitors[i].device == device) return static_cast<int>(i);
	return -1;
}

int DisplayList::MonitorIndexOf(const void* hmonitor) const {
	if (!hmonitor) return -1;
	for (size_t i = 0; i < monitors.size(); ++i)
		if (monitors[i].monitor == hmonitor) return static_cast<int>(i);
	return -1;
}

int DisplayList::AdapterIndex(u64 luid) const {
	for (size_t i = 0; i < adapters.size(); ++i)
		if (adapters[i].luid == luid) return static_cast<int>(i);
	return -1;
}

DisplayList EnumerateDisplays() {
	DisplayList result;

	ComPtr<IDXGIFactory6> factory;
	if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
			DXGI_ADAPTER_DESC1 desc{};
			adapter->GetDesc1(&desc);
			if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
				AdapterInfo info;
				info.luid = PackLuid(desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
				info.identity = IdentityOf(desc);
				info.name = info.identity.description;
				result.adapters.push_back(info);
			}

			// Its outputs join the ONE monitor list, whichever GPU they hang off
			// (a software adapter's too - a GPU-less VM's display hangs off one).
			// A monitor two adapters both report is listed once.
			ComPtr<IDXGIOutput> output;
			for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
				DXGI_OUTPUT_DESC odesc{};
				output->GetDesc(&odesc);
				OutputInfo out;
				out.device = str::Narrow(odesc.DeviceName);
				out.adapter = str::Narrow(desc.Description);
				if (result.MonitorIndex(out.device) >= 0) {
					output.Reset();
					continue;
				}
				out.x = odesc.DesktopCoordinates.left;
				out.y = odesc.DesktopCoordinates.top;
				out.width = odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left;
				out.height = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;
				// The work area and the primary flag from the monitor itself; the
				// whole desktop rect if it will not say (a window then fits the
				// screen, at least).
				if (!ReadMonitor(odesc.Monitor, out)) {
					out.monitor = odesc.Monitor;
					out.workX = out.x;
					out.workY = out.y;
					out.workWidth = out.width;
					out.workHeight = out.height;
				}
				// Supported resolutions for the back-buffer format.
				UINT count = 0;
				output->GetDisplayModeList(kBackBufferFormat, 0, &count, nullptr);
				if (count > 0) {
					std::vector<DXGI_MODE_DESC> modes(count);
					if (SUCCEEDED(output->GetDisplayModeList(kBackBufferFormat, 0, &count,
															 modes.data())))
						for (UINT m = 0; m < count; ++m) AddMode(out, modes[m].Width, modes[m].Height);
				}
				FinishModes(out);
				result.monitors.push_back(std::move(out));
				output.Reset();
			}
			adapter.Reset();
		}
	}

	// Any monitor Windows has that no adapter reported (a display no DXGI
	// adapter claims - an indirect or remote one), with GDI's modes.
	std::vector<HMONITOR> hmons;
	EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&hmons));
	for (HMONITOR h : hmons) {
		if (result.MonitorIndexOf(h) >= 0) continue;
		OutputInfo out;
		if (!ReadMonitor(h, out) || result.MonitorIndex(out.device) >= 0) continue;
		ReadGdiModes(str::Widen(out.device), out);
		FinishModes(out);
		result.monitors.push_back(std::move(out));
	}

	// The primary first, then the order they were found in; named by place.
	std::stable_partition(result.monitors.begin(), result.monitors.end(),
						  [](const OutputInfo& m) { return m.primary; });
	for (size_t i = 0; i < result.monitors.size(); ++i) {
		OutputInfo& m = result.monitors[i];
		m.name = std::format("Display {} ({}x{})", i + 1, m.width, m.height);
	}
	return result;
}

u64 ResolveAdapterLuid(std::string_view savedIdentity) {
	if (savedIdentity.empty()) return 0;
	AdapterIdentity saved;
	if (!DecodeAdapterIdentity(savedIdentity, saved)) {
		log::Warn("settings: adapter_id '{}' does not read - the GPU is picked automatically",
				  savedIdentity);
		return 0;
	}
	const DisplayList list = EnumerateDisplays();
	std::vector<AdapterIdentity> installed;
	installed.reserve(list.adapters.size());
	for (const AdapterInfo& a : list.adapters) installed.push_back(a.identity);
	const int at = ResolveAdapterIdentity(saved, installed);
	if (at < 0) {
		log::Warn("settings: the saved GPU ({}) is not installed - the GPU is picked "
				  "automatically",
				  savedIdentity);
		return 0;
	}
	log::Info("settings: the saved GPU resolves to {} (luid {:016x})",
			  list.adapters[static_cast<size_t>(at)].name, list.adapters[static_cast<size_t>(at)].luid);
	return list.adapters[static_cast<size_t>(at)].luid;
}

} // namespace dungeon::gfx
