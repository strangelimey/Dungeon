// ============================================================================
// Game/DevConsole.cpp — see DevConsole.h.
// ============================================================================
#include "Game/DevConsole.h"

#include "UI/Controls.h" // ui::DrawBorder

#include <Windows.h> // VK_* codes

#include <algorithm>
#include <format>
#include <sstream>

namespace dungeon::game {

namespace {
constexpr float kDesignWindowH = 900.0f; // font authored against this height
constexpr float kFontH = 16.0f;          // console font px at the design height
constexpr size_t kMaxOutput = 500;       // scrollback cap

// Console palette (dev-facing, not themed).
const Vec4 kBackground{0.03f, 0.03f, 0.05f, 0.92f};
const Vec4 kPerfBg{0.06f, 0.06f, 0.09f, 1.0f};
const Vec4 kBorder{0.35f, 0.38f, 0.48f, 1.0f};
const Vec4 kText{0.85f, 0.88f, 0.92f, 1.0f};
const Vec4 kDim{0.55f, 0.58f, 0.66f, 1.0f};
const Vec4 kAccent{0.55f, 0.85f, 0.55f, 1.0f};
const Vec4 kGaugeBg{0.13f, 0.13f, 0.17f, 1.0f};

std::vector<std::string> Tokenize(const std::string& line) {
	std::vector<std::string> tokens;
	std::istringstream stream(line);
	std::string token;
	while (stream >> token) tokens.push_back(token);
	return tokens;
}
} // namespace

DevConsole::DevConsole(gfx::GraphicsDevice& device) : m_font(device, "", kFontH) {
	// Generic built-ins. Gameplay-aware commands are registered by the Game.
	Register("help", "list available commands", [this](const std::vector<std::string>&) {
		for (const Command& cmd : m_commands)
			Print(std::format("  {:<10} {}", cmd.name, cmd.help));
	});
	Register("clear", "clear the console output", [this](const std::vector<std::string>&) {
		m_output.clear();
		m_scroll = 0;
	});
	Register("echo", "echo the arguments", [this](const std::vector<std::string>& args) {
		std::string line;
		for (size_t i = 0; i < args.size(); ++i)
			line += (i ? " " : "") + args[i];
		Print(line);
	});

	Print("Developer console - type 'help' for commands.");
}

void DevConsole::Toggle() {
	m_open = !m_open;
	if (m_open) {
		m_scroll = 0;
		m_caretBlink = 0.0f;
		m_historyIndex = -1;
	}
}

void DevConsole::Register(std::string name, std::string help,
						  std::function<void(const std::vector<std::string>&)> fn) {
	m_commands.push_back({std::move(name), std::move(help), std::move(fn)});
}

void DevConsole::Print(std::string line) {
	m_output.push_back(std::move(line));
	while (m_output.size() > kMaxOutput) m_output.pop_front();
	m_scroll = 0; // jump to the newest line
}

void DevConsole::Execute(const std::string& line) {
	Print("> " + line);
	const std::vector<std::string> tokens = Tokenize(line);
	if (tokens.empty()) return;

	std::string name = tokens[0];
	std::ranges::transform(name, name.begin(), [](char c) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	});
	const std::vector<std::string> args(tokens.begin() + 1, tokens.end());

	for (const Command& cmd : m_commands) {
		if (cmd.name == name) {
			cmd.fn(args);
			return;
		}
	}
	Print("unknown command: " + name);
}

void DevConsole::Update(const Input& input, float dt, float windowW, float windowH) {
	m_perf.Tick(dt);
	if (!m_open) return;

	m_caretBlink += dt;
	(void)windowW;
	m_font.SetHeight(kFontH * (windowH / kDesignWindowH));

	// Typed characters (skip the toggle key so `~`/backtick never self-types).
	for (char c : input.TypedChars())
		if (c != '`' && c != '~') m_input.push_back(c);

	if (input.WasKeyPressed(VK_BACK) && !m_input.empty()) m_input.pop_back();

	if (input.WasKeyPressed(VK_RETURN)) {
		if (!m_input.empty()) {
			m_history.push_back(m_input);
			Execute(m_input);
			m_input.clear();
		}
		m_historyIndex = -1;
	}

	// Command history recall.
	if (input.WasKeyPressed(VK_UP) && !m_history.empty()) {
		if (m_historyIndex == -1)
			m_historyIndex = static_cast<int>(m_history.size()) - 1;
		else if (m_historyIndex > 0)
			--m_historyIndex;
		m_input = m_history[static_cast<size_t>(m_historyIndex)];
	}
	if (input.WasKeyPressed(VK_DOWN) && m_historyIndex != -1) {
		if (m_historyIndex < static_cast<int>(m_history.size()) - 1) {
			++m_historyIndex;
			m_input = m_history[static_cast<size_t>(m_historyIndex)];
		} else {
			m_historyIndex = -1;
			m_input.clear();
		}
	}

	// Scroll the output (wheel, or PageUp/PageDown).
	int scrollLines = static_cast<int>(input.WheelDelta());
	if (input.WasKeyPressed(VK_PRIOR)) scrollLines += 5;
	if (input.WasKeyPressed(VK_NEXT)) scrollLines -= 5;
	if (scrollLines != 0) {
		m_scroll = std::clamp(m_scroll + scrollLines, 0,
							  static_cast<int>(m_output.size()));
		m_caretBlink = 0.0f;
	}

	if (input.WasKeyPressed(VK_ESCAPE)) m_open = false;
}

void DevConsole::Render(gfx::SpriteBatch& batch, const gfx::GraphicsDevice& device,
						float width, float height) {
	const float line = m_font.LineAdvance();
	const float pad = line * 0.5f;

	// Full-screen dim background.
	batch.DrawRect({0, 0, width, height}, kBackground);

	// --- performance panel (top) --------------------------------------------
	const PerfMonitor::Metrics& m = m_perf.Get();
	const gfx::GraphicsDevice::GpuMemoryInfo vram = device.QueryGpuMemory();
	const double gpuUsedGB = static_cast<double>(vram.usedBytes) / (1024.0 * 1024.0 * 1024.0);
	const double gpuBudgetGB = static_cast<double>(vram.budgetBytes) / (1024.0 * 1024.0 * 1024.0);

	const float panelH = line * 8.0f + pad * 2.0f;
	batch.DrawRect({0, 0, width, panelH}, kPerfBg);
	ui::DrawBorder(batch, {0, 0, width, panelH}, kBorder);

	const float labelX = pad * 2.0f;
	const float gaugeX = width * 0.40f;
	const float gaugeW = width * 0.28f;
	float y = pad;

	auto gauge = [&](float gy, float frac, const Vec4& fill) {
		const float gh = line * 0.7f;
		const float oy = gy + (line - gh) * 0.5f;
		batch.DrawRect({gaugeX, oy, gaugeW, gh}, kGaugeBg);
		batch.DrawRect({gaugeX, oy, gaugeW * std::clamp(frac, 0.0f, 1.0f), gh}, fill);
		ui::DrawBorder(batch, {gaugeX, oy, gaugeW, gh}, kBorder);
	};
	auto row = [&](const std::string& text) {
		m_font.Draw(batch, text, labelX, y, kText);
		y += line;
	};

	m_font.Draw(batch, "PERFORMANCE", labelX, y, kAccent);
	m_font.Draw(batch, std::format("FPS {:.0f}", m.fps), gaugeX, y, kAccent);
	y += line;

	m_font.Draw(batch, std::format("CPU  {:.0f}%", m.cpuPercent), labelX, y, kText);
	gauge(y, m.cpuPercent / 100.0f, {0.45f, 0.70f, 0.95f, 1.0f});
	y += line;

	if (m.gpuPercent >= 0.0f) {
		m_font.Draw(batch, std::format("GPU  {:.0f}%", m.gpuPercent), labelX, y, kText);
		gauge(y, m.gpuPercent / 100.0f, {0.55f, 0.85f, 0.55f, 1.0f});
	} else {
		m_font.Draw(batch, "GPU  n/a", labelX, y, kDim);
	}
	y += line;

	const float sysFrac = m.sysMemTotalMB > 0
							   ? static_cast<float>(m.sysMemUsedMB / m.sysMemTotalMB)
							   : 0.0f;
	m_font.Draw(batch,
				std::format("RAM  {:.1f} / {:.1f} GB", m.sysMemUsedMB / 1024.0,
							m.sysMemTotalMB / 1024.0),
				labelX, y, kText);
	gauge(y, sysFrac, {0.85f, 0.70f, 0.40f, 1.0f});
	y += line;

	const float vramFrac = gpuBudgetGB > 0 ? static_cast<float>(gpuUsedGB / gpuBudgetGB) : 0.0f;
	m_font.Draw(batch, std::format("VRAM {:.2f} / {:.2f} GB", gpuUsedGB, gpuBudgetGB),
				labelX, y, kText);
	gauge(y, vramFrac, {0.80f, 0.55f, 0.85f, 1.0f});
	y += line;

	row(std::format("Process working set: {:.0f} MB", m.procMemMB));
	row("GPU: " + device.AdapterName());

	// --- output log + input line (bottom) -----------------------------------
	const float inputY = height - line - pad;
	const float promptW = m_font.MeasureWidth("> ");
	m_font.Draw(batch, "> ", labelX, inputY, kAccent);
	m_font.Draw(batch, m_input, labelX + promptW, inputY, kText);
	if (std::fmod(m_caretBlink, 1.0f) < 0.5f) {
		const float caretX = labelX + promptW + m_font.MeasureWidth(m_input);
		batch.DrawRect({caretX + 1.0f, inputY, 2.0f, line}, kText);
	}
	batch.DrawRect({0, inputY - pad * 0.5f, width, 1.0f}, kBorder);

	// Scrollback, newest at the bottom just above the input line.
	const float logTop = panelH + pad;
	const float logBottom = inputY - pad;
	int visible = static_cast<int>((logBottom - logTop) / line);
	if (visible < 0) visible = 0;
	const int total = static_cast<int>(m_output.size());
	const int end = std::max(0, total - m_scroll); // index past the last shown
	const int start = std::max(0, end - visible);
	float ly = logBottom - line;
	for (int i = end - 1; i >= start; --i) {
		m_font.Draw(batch, m_output[static_cast<size_t>(i)], labelX, ly, kText);
		ly -= line;
	}
}

} // namespace dungeon::game
