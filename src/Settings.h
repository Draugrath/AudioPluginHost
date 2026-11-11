#pragma once

#include "PCH.h"

class Settings
{
public:
	struct SlotEntry
	{
		bool        on{ true };
		std::string uid;
		std::string path;
	};

	static Settings& GetSingleton()
	{
		static Settings instance;
		return instance;
	}

	static constexpr const char* kPath = "Data/SKSE/Plugins/AudioPluginHost.ini";
	static constexpr const char* kFolder = "Data/SKSE/Plugins/AudioPluginHost";

	static std::filesystem::path StatePath(std::size_t a_index) { return std::filesystem::path(kFolder) / std::format("Slot{}.state", a_index); }

	void Load()
	{
		std::ifstream file(kPath);
		std::string   line;
		while (std::getline(file, line)) {
			const auto equals = line.find('=');
			if (line.empty() || line[0] == '#' || line[0] == ';' || equals == std::string::npos) {
				continue;
			}
			const auto key = Trim(line.substr(0, equals));
			const auto value = Trim(line.substr(equals + 1));
			try {
				if (key == "bEnabled") {
					enabled = value == "true" || value == "1";
				} else if (key == "bEditorsOnTop") {
					editorsOnTop = value == "true" || value == "1";
				} else if (key == "bStereoMaster") {
					stereoMaster = value == "true" || value == "1";
				} else if (key == "iToggleKey") {
					toggleKey = static_cast<std::uint32_t>(std::stoul(value));
				} else if (key == "fWindowScale") {
					windowScale = std::clamp(std::stof(value), 0.5f, 3.0f);
				} else if (key == "sExtraFolder") {
					extraFolder = value;
				} else if (key.starts_with("Slot")) {
					const auto first = value.find(',');
					const auto second = first == std::string::npos ? std::string::npos : value.find(',', first + 1);
					if (second != std::string::npos) {
						slots.push_back({ Trim(value.substr(0, first)) != "0", Trim(value.substr(first + 1, second - first - 1)), Trim(value.substr(second + 1)) });
					}
				}
			} catch (...) {
			}
		}
	}

	void Save() const
	{
		std::ofstream file(kPath, std::ios::trunc);
		file << "# Audio Plugin Host settings. The in-game Save button rewrites this file.\n\n";
		file << "# Turns all plugins on or off.\nbEnabled = " << (enabled ? "true" : "false") << "\n\n";
		file << "# Keeps plugin windows above the game.\nbEditorsOnTop = " << (editorsOnTop ? "true" : "false") << "\n\n";
		file << "# For stereo speakers or headphones: the plugins process the final stereo mix instead of the game's 6 channels.\nbStereoMaster = " << (stereoMaster ? "true" : "false") << "\n\n";
		file << "# Key that opens the menu, as a Windows key code (117 = F6).\niToggleKey = " << toggleKey << "\n\n";
		file << "# Size of the window: 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5 or 3 (2 suits 4K screens).\nfWindowScale = " << windowScale << "\n\n";
		file << "# Another folder to search for .vst3 plugins, besides C:\\Program Files\\Common Files\\VST3 and SKSE\\Plugins\\AudioPluginHost\\VST3.\nsExtraFolder = " << extraFolder << "\n\n";
		file << "# Each slot in processing order: on (0 or 1), the plugin's class ID, its file. The plugin's own settings are in AudioPluginHost\\SlotN.state.\n";
		for (std::size_t i = 0; i < slots.size(); ++i) {
			file << "Slot" << i << " = " << (slots[i].on ? 1 : 0) << ", " << slots[i].uid << ", " << slots[i].path << "\n";
		}
	}

	bool                   enabled{ true };
	bool                   editorsOnTop{ true };
	bool                   stereoMaster{ false };
	std::uint32_t          toggleKey{ VK_F6 };
	float                  windowScale{ 1.0f };
	std::string            extraFolder;
	std::vector<SlotEntry> slots;

private:
	static std::string Trim(std::string a_text)
	{
		const auto first = a_text.find_first_not_of(" \t\r");
		const auto last = a_text.find_last_not_of(" \t\r");
		return first == std::string::npos ? std::string() : a_text.substr(first, last - first + 1);
	}
};
