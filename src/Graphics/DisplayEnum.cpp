// ============================================================================
// Graphics/DisplayEnum.cpp — see DisplayEnum.h.
// ============================================================================
#include "Graphics/DisplayEnum.h"

#include "Core/StringUtil.h"
#include "Graphics/GraphicsDevice.h" // kBackBufferFormat
#include "Graphics/D3DUtil.h"        // ComPtr

#include <dxgi1_6.h>

#include <algorithm>
#include <format>

namespace dungeon::gfx {

u64 PackLuid(i32 highPart, u32 lowPart) {
	return (static_cast<u64>(static_cast<u32>(highPart)) << 32) | lowPart;
}

std::vector<AdapterInfo> EnumerateAdapters() {
	std::vector<AdapterInfo> result;

	ComPtr<IDXGIFactory6> factory;
	if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return result;

	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 desc{};
		adapter->GetDesc1(&desc);
		if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
			AdapterInfo info;
			info.luid = PackLuid(desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
			info.name = str::Narrow(desc.Description);

			// Outputs (monitors) on this adapter.
			ComPtr<IDXGIOutput> output;
			for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND;
				 ++o) {
				DXGI_OUTPUT_DESC odesc{};
				output->GetDesc(&odesc);
				OutputInfo out;
				out.x = odesc.DesktopCoordinates.left;
				out.y = odesc.DesktopCoordinates.top;
				out.width = odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left;
				out.height =
					odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;
				out.name = std::format("Display {} ({}x{})", o + 1, out.width, out.height);

				// Supported resolutions for the back-buffer format, deduped to
				// unique (w,h) and sorted largest-first.
				UINT count = 0;
				output->GetDisplayModeList(kBackBufferFormat, 0, &count, nullptr);
				if (count > 0) {
					std::vector<DXGI_MODE_DESC> modes(count);
					output->GetDisplayModeList(kBackBufferFormat, 0, &count, modes.data());
					for (const DXGI_MODE_DESC& m : modes) {
						const bool seen = std::any_of(
							out.modes.begin(), out.modes.end(), [&](const DisplayMode& d) {
								return d.width == m.Width && d.height == m.Height;
							});
						if (!seen) out.modes.push_back({m.Width, m.Height});
					}
					std::sort(out.modes.begin(), out.modes.end(),
							  [](const DisplayMode& a, const DisplayMode& b) {
								  return a.width * a.height > b.width * b.height;
							  });
				}
				// Fall back to the current desktop size if the driver listed none.
				if (out.modes.empty() && out.width > 0)
					out.modes.push_back(
						{static_cast<u32>(out.width), static_cast<u32>(out.height)});

				info.outputs.push_back(std::move(out));
			}

			result.push_back(std::move(info));
		}
		adapter.Reset();
	}
	return result;
}

} // namespace dungeon::gfx
