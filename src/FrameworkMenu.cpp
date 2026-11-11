#include "FrameworkMenu.h"
#include "Host.h"
#include "Rack.h"
#include "Settings.h"
#include "Version.h"

#include <SKSEMenuFramework.h>

namespace Menu
{
	using namespace ImGuiMCP;

	namespace
	{
		MENU_WINDOW       window = nullptr;
		std::atomic<bool> waitingForKey{ false };
		float             fontScale = 0.0f;

		bool        enabled = true;
		bool        editorsOnTop = true;
		bool        stereoMaster = false;
		float       windowScale = 1.0f;
		std::string loadText;
		std::string statusText;
		std::string folderText;
		std::size_t chosen = 0;
		ULONGLONG   loadAt = 0;

		const ImVec4 kGold{ 0.78f, 0.65f, 0.36f, 1.0f };
		const ImVec4 kRed{ 0.90f, 0.40f, 0.35f, 1.0f };

		std::uint32_t KeyToScan(std::uint32_t a_key)
		{
			const auto scan = MapVirtualKeyW(a_key, MAPVK_VK_TO_VSC_EX);
			return (scan >> 8) == 0xE0 ? (scan & 0xFF) | 0x80 : scan & 0xFF;
		}

		std::uint32_t ScanToKey(std::uint32_t a_scan)
		{
			return MapVirtualKeyW(a_scan >= 0x80 ? 0xE000 | (a_scan & 0x7F) : a_scan, MAPVK_VSC_TO_VK_EX);
		}

		std::string KeyName(std::uint32_t a_key)
		{
			const auto scan = KeyToScan(a_key);
			char       name[64]{};
			const LONG code = static_cast<LONG>(((scan & 0x7F) << 16) | (scan >= 0x80 ? 1 << 24 : 0));
			return GetKeyNameTextA(code, name, sizeof(name)) > 0 ? std::string(name) : std::format("key {}", a_key);
		}

		void Tip(const char* a_text)
		{
			if (IsItemHovered(0)) {
				SetTooltip("%s", a_text);
			}
		}

		bool Zoom(float& a_scale, float a_width)
		{
			static constexpr float       kScales[]{ 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f };
			static constexpr const char* kNames[]{ "75%", "100%", "125%", "150%", "175%", "200%", "250%", "300%" };
			int                          index = 0;
			for (int i = 1; i < 8; ++i) {
				if (std::fabs(kScales[i] - a_scale) < std::fabs(kScales[index] - a_scale)) {
					index = i;
				}
			}
			SetNextItemWidth(a_width);
			if (!Combo("Window Size", &index, kNames, 8, -1)) {
				return false;
			}
			a_scale = kScales[index];
			return true;
		}

		void SaveSettings()
		{
			auto& settings = Settings::GetSingleton();
			settings.windowScale = windowScale;
			settings.editorsOnTop = editorsOnTop;
			settings.stereoMaster = stereoMaster;
			settings.enabled = enabled;
			Host::SaveSettings();
		}

		std::string Milliseconds(std::int32_t a_samples, double a_rate)
		{
			if (a_samples <= 0 || a_rate <= 0.0) {
				return "0 ms";
			}
			return std::format("{:.1f} ms", a_samples * 1000.0 / a_rate);
		}

		struct MeterState
		{
			float     level[Vst::kMaxChannels];
			float     hold[Vst::kMaxChannels];
			ULONGLONG holdAt[Vst::kMaxChannels];
			ULONGLONG clipAt[Vst::kMaxChannels];
			bool      started{ false };
		};
		MeterState meter;
		ULONGLONG  meterDrawn = 0;

		const char* ChannelName(std::uint32_t a_count, std::uint32_t a_channel)
		{
			static constexpr const char* kStereo[]{ "L", "R" };
			static constexpr const char* kSurround[]{ "L", "R", "C", "LFE", "Ls", "Rs", "Lb", "Rb" };
			static constexpr const char* kNumbers[]{ "1", "2", "3", "4", "5", "6", "7", "8" };
			if (a_count == 2) {
				return kStereo[a_channel];
			}
			return a_count == 6 || a_count == 8 ? kSurround[a_channel] : kNumbers[a_channel];
		}

		float DrawMeter(float a_scale)
		{
			constexpr float kFloor = -60.0f;
			float           peaks[Vst::kMaxChannels]{};
			const auto      channels = (std::min)(Rack::ReadMeter(peaks), Vst::kMaxChannels);
			const auto      now = GetTickCount64();
			if (!meter.started || now - meterDrawn > 1000) {
				for (std::uint32_t c = 0; c < Vst::kMaxChannels; ++c) {
					meter.level[c] = meter.hold[c] = -120.0f;
					meter.holdAt[c] = meter.clipAt[c] = 0;
				}
				meter.started = true;
			}
			meterDrawn = now;
			if (channels == 0) {
				TextDisabled("Waiting for the game's sound...");
				return -120.0f;
			}

			SetWindowFontScale(fontScale * a_scale * 0.8f);
			auto*        list = GetWindowDrawList();
			const ImVec2 origin = GetCursorScreenPos();
			const float  line = GetTextLineHeight();
			const float  bar = (std::max)(3.0f, 5.0f * a_scale);
			const float  pitch = (std::max)(line - 2.0f * a_scale, bar + 3.0f * a_scale);
			const float  labelWidth = CalcTextSize("LFE", nullptr, false, -1.0f).x + 6.0f * a_scale;
			const float  valueWidth = CalcTextSize("-00.0 dB", nullptr, false, -1.0f).x + 6.0f * a_scale;
			const float  width = (std::min)(GetContentRegionAvail().x, 360.0f * a_scale);
			const float  left = origin.x + labelWidth;
			const float  right = origin.x + width - valueWidth;
			const float  span = (std::max)(10.0f, right - left);
			const auto   at = [&](float a_db) { return left + span * std::clamp((a_db - kFloor) / -kFloor, 0.0f, 1.0f); };
			const float  fall = 30.0f * (std::min)(GetIO()->DeltaTime, 0.25f);

			const ImU32 kBack = IM_COL32(255, 255, 255, 18);
			const ImU32 kGreen = IM_COL32(96, 176, 104, 255);
			const ImU32 kYellow = IM_COL32(206, 178, 84, 255);
			const ImU32 kRedBar = IM_COL32(214, 92, 78, 255);
			const ImU32 kTick = IM_COL32(235, 235, 228, 255);
			const ImU32 kGrid = IM_COL32(0, 0, 0, 150);
			const ImU32 kDim = IM_COL32(150, 150, 145, 255);
			const ImU32 kText = IM_COL32(204, 204, 199, 255);

			float loudest = -120.0f;
			for (std::uint32_t c = 0; c < channels; ++c) {
				const float db = peaks[c] > 1e-6f ? 20.0f * std::log10(peaks[c]) : -120.0f;
				meter.level[c] = (std::max)(db, meter.level[c] - fall);
				if (db >= meter.hold[c] || now - meter.holdAt[c] > 1500) {
					meter.hold[c] = (std::max)(db, meter.level[c]);
					meter.holdAt[c] = now;
				}
				if (peaks[c] >= 1.0f) {
					meter.clipAt[c] = now;
				}
				loudest = (std::max)(loudest, meter.hold[c]);
				const float y = origin.y + pitch * static_cast<float>(c);
				const float top = y + (pitch - bar) * 0.5f;
				const float textY = y + (pitch - line) * 0.5f;
				const float level = meter.level[c];
				ImDrawListManager::AddText(list, ImVec2(origin.x, textY), kDim, ChannelName(channels, c), nullptr);
				ImDrawListManager::AddRectFilled(list, ImVec2(left, top), ImVec2(right, top + bar), kBack, 0.0f, 0);
				if (level > kFloor) {
					ImDrawListManager::AddRectFilled(list, ImVec2(left, top), ImVec2(at((std::min)(level, -12.0f)), top + bar), kGreen, 0.0f, 0);
				}
				if (level > -12.0f) {
					ImDrawListManager::AddRectFilled(list, ImVec2(at(-12.0f), top), ImVec2(at((std::min)(level, -3.0f)), top + bar), kYellow, 0.0f, 0);
				}
				if (level > -3.0f) {
					ImDrawListManager::AddRectFilled(list, ImVec2(at(-3.0f), top), ImVec2(at(level), top + bar), kRedBar, 0.0f, 0);
				}
				if (meter.hold[c] > kFloor) {
					const float x = at(meter.hold[c]);
					ImDrawListManager::AddRectFilled(list, ImVec2(x - 1.0f, top), ImVec2(x + 1.0f, top + bar), kTick, 0.0f, 0);
				}
				const bool clipped = now - meter.clipAt[c] < 2000;
				const auto value = clipped ? std::string("CLIP") : meter.hold[c] > -100.0f ? std::format("{:.1f} dB", meter.hold[c]) : std::string("-");
				ImDrawListManager::AddText(list, ImVec2(right + 6.0f * a_scale, textY), clipped ? kRedBar : meter.hold[c] > -100.0f ? kText : kDim, value.c_str(), nullptr);
			}

			const float bottom = origin.y + pitch * static_cast<float>(channels);
			for (const int mark : { -48, -36, -24, -12, -6, 0 }) {
				const float x = at(static_cast<float>(mark));
				if (mark != 0) {
					ImDrawListManager::AddRectFilled(list, ImVec2(x, origin.y + 1.0f), ImVec2(x + 1.0f, bottom - 1.0f), kGrid, 0.0f, 0);
				}
				const auto   label = std::to_string(mark);
				const ImVec2 size = CalcTextSize(label.c_str(), nullptr, false, -1.0f);
				ImDrawListManager::AddText(list, ImVec2(mark == 0 ? right - size.x : x - size.x * 0.5f, bottom), kDim, label.c_str(), nullptr);
			}
			Dummy(ImVec2(width, bottom + line - origin.y));
			SetWindowFontScale(fontScale * a_scale);
			if (IsItemHovered(0)) {
				SetTooltip("%s", "The level of each channel after the plugins, in dB below full scale (0 = the loudest possible).\n"
				                 "The white mark is the highest level of the last moments. CLIP: a plugin pushed the sound over full scale.\n"
				                 "Audio Equalizer and Dynamic Compressor, if installed, come after this point.");
			}
			return loudest;
		}

		void SlotRow(const Rack::View& a_view, std::size_t a_index)
		{
			const auto& slot = a_view.slots[a_index];
			const bool  live = slot.loaded && slot.on && !slot.crashed && enabled;
			PushID(static_cast<int>(a_index));
			TableNextRow(0, 0.0f);

			TableNextColumn();
			Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, GetFrameHeight()));
			if (BeginDragDropSource(0)) {
				const int from = static_cast<int>(a_index);
				SetDragDropPayload("APH_SLOT", &from, sizeof(from), 0);
				Text("Move %s", slot.name.c_str());
				EndDragDropSource();
			}
			if (BeginDragDropTarget()) {
				if (const auto* payload = AcceptDragDropPayload("APH_SLOT", 0); payload && payload->DataSize == sizeof(int)) {
					const int from = *static_cast<const int*>(payload->Data);
					if (from >= 0) {
						Rack::MoveTo(static_cast<std::size_t>(from), a_index);
					}
				}
				EndDragDropTarget();
			}
			if (IsItemHovered(0) && !IsMouseDragging(0, -1.0f)) {
				SetTooltip("%s", "Drag to change the order. Sound goes through the plugins from top to bottom.");
			}
			SameLine(0.0f, -1.0f);
			bool on = slot.on;
			if (Checkbox("##on", &on)) {
				Rack::SetOn(a_index, on);
			}
			Tip("Turns this plugin on or off.");

			TableNextColumn();
			AlignTextToFramePadding();
			if (slot.crashed) {
				TextColored(kRed, "%zu  %s (crashed, switched off)", a_index + 1, slot.name.c_str());
			} else if (!slot.loaded) {
				TextColored(kRed, "%zu  %s (not loaded)", a_index + 1, slot.name.c_str());
			} else if (live) {
				TextColored(kGold, "%zu  %s", a_index + 1, slot.name.c_str());
			} else {
				TextDisabled("%zu  %s", a_index + 1, slot.name.c_str());
			}
			if (!slot.error.empty()) {
				Tip(slot.error.c_str());
			}

			TableNextColumn();
			AlignTextToFramePadding();
			if (slot.loaded) {
				TextDisabled("%s", slot.vendor.c_str());
			}

			TableNextColumn();
			AlignTextToFramePadding();
			if (slot.loaded) {
				TextDisabled("%s", slot.layout.c_str());
				Tip("How the game's 6 channels (front left/right, center, bass, rear left/right) go through the plugin.\n"
				    "5.1: one copy of the plugin gets all 6.\n"
				    "3 x stereo: the plugin has no 5.1 mode, so three linked copies take front, center and rear; the bass channel passes by.\n"
				    "stereo: Stereo master is on, so one stereo copy gets the final mix.\n"
				    "Settings you change in its window apply to every copy.");
			}

			TableNextColumn();
			AlignTextToFramePadding();
			if (slot.loaded) {
				const auto delay = Milliseconds(slot.latency, a_view.rate);
				if (slot.latency > 0 && live) {
					Text("%s", delay.c_str());
				} else {
					TextDisabled("%s", delay.c_str());
				}
				if (IsItemHovered(0)) {
					SetTooltip("This plugin lets the sound out %d samples later than it came in.\nIt only counts while the plugin is on.", slot.latency);
				}
			}

			TableNextColumn();
			AlignTextToFramePadding();
			if (slot.loaded) {
				if (live && slot.cpu >= 0.05f) {
					Text("%.1f%%", slot.cpu);
				} else {
					TextDisabled("%.1f%%", live ? slot.cpu : 0.0f);
				}
				Tip("Share of one processor core this plugin uses while the game plays sound.");
			}

			TableNextColumn();
			TableNextColumn();
			BeginDisabled(!slot.hasEditor);
			if (Button(slot.editorOpen ? "Close window" : "Open window")) {
				Rack::ToggleEditor(a_index);
			}
			EndDisabled();
			SameLine(0.0f, -1.0f);
			if (Button("Remove")) {
				Rack::Remove(a_index);
			}
			PopID();
		}

		void LatencyLine(const Rack::View& a_view)
		{
			const double ms = a_view.rate > 0.0 ? a_view.latency * 1000.0 / a_view.rate : 0.0;
			AlignTextToFramePadding();
			if (a_view.latency <= 0) {
				TextDisabled("Total latency  0 ms.  The game's sound is not delayed.");
			} else {
				const float fps = std::clamp(GetIO()->Framerate, 10.0f, 1000.0f);
				const char* verdict = ms <= 10.0 ? "too short to notice" : ms <= 30.0 ? "very hard to notice" : ms <= 60.0 ? "small; sharp ears may catch sounds a little after the picture" : "noticeable: sounds lag behind the picture";
				TextColored(kGold, "Total latency  %.1f ms", ms);
				SameLine(0.0f, -1.0f);
				TextDisabled("= %.1f frames at %.0f fps, like hearing everything from %.0f m further away (%s)", ms * fps / 1000.0, fps, ms * 0.343, verdict);
			}
			if (IsItemHovered(0)) {
				SetTooltip("Every sound in the game reaches you this much later than without the plugins: %d samples at %.0f Hz.\n"
				           "It is the latency of all plugins that are on, added up. Skyrim does not know about it and does not\n"
				           "move the picture to match, so footsteps, hits and voices come that much after what you see.\n"
				           "To lower it: switch off or remove plugins with latency, or pick a low- or zero-latency mode inside the plugin.",
					a_view.latency, a_view.rate);
			}
		}

		void Render()
		{
			const auto  held = Rack::Peek();
			const auto& view = *held;
			const float scale = windowScale;
			const auto  now = GetTickCount64();
			Rack::SetMetering(true);

			if (Checkbox("Enabled", &enabled)) {
				Rack::SetEnabled(enabled);
				SaveSettings();
			}
			Tip("Turns every plugin on or off. Switching is a short fade, without clicks.");
			SameLine(0.0f, -1.0f);
			if (Button("Save")) {
				Rack::Save(true);
			}
			Tip("Saves the list and every plugin's own settings. They load again with the game.");
			SameLine(0.0f, -1.0f);
			if (Checkbox("Windows on top", &editorsOnTop)) {
				Rack::SetEditorsOnTop(editorsOnTop);
				SaveSettings();
			}
			Tip("Keeps plugin windows above the game.");
			SameLine(0.0f, -1.0f);
			const bool stereoPossible = Host::StereoAvailable();
			BeginDisabled(!stereoPossible);
			bool stereoShown = stereoPossible && stereoMaster;
			if (Checkbox("Stereo master", &stereoShown)) {
				stereoMaster = stereoShown;
				Rack::SetStereo(stereoMaster);
				SaveSettings();
			}
			EndDisabled();
			if (IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				const auto note = Host::StereoNote();
				SetTooltip("%s%s", "For stereo speakers or headphones. Skyrim mixes its sound in 6 channels, and its audio engine folds them\n"
				                   "to stereo at the very end, after the plugins.\n"
				                   "On: the plugins process that final stereo mix, one stereo copy each (plugins without a 5.1 mode use about\n"
				                   "a third of the CPU). The meter shows left and right as you hear them.\n"
				                   "Off: the plugins process the game's 6 channels.\n"
				                   "Without plugins both sound the same. Every plugin loads again when you switch, with its settings.\n"
				                   "With Audio Equalizer or Dynamic Compressor installed: they then get all sound on the front pair, about 10 dB\n"
				                   "louder than before, so their meters and limiter read higher.",
					note.empty() ? "" : ("\n\n" + note).c_str());
			}
			SameLine(0.0f, -1.0f);
			if (Zoom(windowScale, 80.0f * scale)) {
				SaveSettings();
			}
			if (!view.message.empty()) {
				SameLine(0.0f, -1.0f);
				TextDisabled("%s", view.message.c_str());
			}

			SeparatorText("Chain");
			if (!view.started) {
				TextDisabled("Starting...");
			} else if (view.slots.empty()) {
				TextDisabled("No plugins yet. Add one below.");
			} else if (BeginTable("##slots", 8, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg, ImVec2(0.0f, 0.0f), 0.0f)) {
				TableSetupColumn("On", 0, 0.0f, 0);
				TableSetupColumn("Plugin", 0, 0.0f, 0);
				TableSetupColumn("Maker", 0, 0.0f, 0);
				TableSetupColumn("Channels", 0, 0.0f, 0);
				TableSetupColumn("Latency", 0, 0.0f, 0);
				TableSetupColumn("CPU", 0, 0.0f, 0);
				TableSetupColumn("##gap", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
				TableSetupColumn("", 0, 0.0f, 0);
				TableHeadersRow();
				for (std::size_t i = 0; i < view.slots.size(); ++i) {
					SlotRow(view, i);
				}
				EndTable();
			}
			if (!view.slots.empty()) {
				LatencyLine(view);
			}

			SeparatorText("Output after the plugins");
			if (now >= loadAt) {
				loadAt = now + 1000;
				loadText = Host::LoadText();
				statusText = Host::Status();
				folderText = Host::Folder();
			}
			BeginGroup();
			const float loudest = DrawMeter(scale);
			EndGroup();
			SameLine(0.0f, 20.0f * scale);
			BeginGroup();
			if (loudest > -100.0f) {
				Text("Peak %.1f dB", loudest);
			} else {
				TextDisabled("Peak -");
			}
			TextDisabled("%s", view.stereo ? "Stereo master: the mix you hear" : "The game's channels, before its mix to your device");
			if (!loadText.empty()) {
				TextDisabled("%s", loadText.c_str());
			}
			EndGroup();

			SeparatorText("Add a plugin");
			if (chosen >= view.available.size()) {
				chosen = 0;
			}
			const auto preview = view.available.empty() ? std::string(view.scanning ? "Scanning..." : "No VST3 plugins found") :
			                                              std::format("{} ({})", view.available[chosen].name, view.available[chosen].vendor);
			SetNextItemWidth(320.0f * scale);
			if (BeginCombo("##plugins", preview.c_str(), 0)) {
				for (std::size_t i = 0; i < view.available.size(); ++i) {
					const auto label = std::format("{} ({})##{}", view.available[i].name, view.available[i].vendor, i);
					if (Selectable(label.c_str(), i == chosen, 0, ImVec2(0.0f, 0.0f))) {
						chosen = i;
					}
				}
				EndCombo();
			}
			SameLine(0.0f, -1.0f);
			BeginDisabled(view.available.empty() || view.busy);
			if (Button("Add")) {
				Rack::Add(view.available[chosen]);
			}
			EndDisabled();
			Tip("Adds the plugin at the end of the chain. Plugins run top to bottom, before Audio Equalizer.");
			SameLine(0.0f, -1.0f);
			BeginDisabled(view.scanning);
			if (Button(view.scanning ? "Scanning..." : "Rescan")) {
				Rack::Rescan();
			}
			EndDisabled();
			Tip("Looks again for .vst3 plugins in C:\\Program Files\\Common Files\\VST3, SKSE\\Plugins\\AudioPluginHost\\VST3 and the folder below.");
			SameLine(0.0f, -1.0f);
			TextDisabled("%zu found", view.available.size());

			const auto& folder = folderText;
			const bool choosing = Host::Choosing();
			BeginDisabled(choosing);
			if (Button(choosing ? "Choosing..." : "Choose folder")) {
				Host::ChooseFolder();
				loadAt = 0;
			}
			EndDisabled();
			Tip("Opens the Windows folder dialog. The plugins in the folder you pick show up in the list above.");
			if (!folder.empty()) {
				SameLine(0.0f, -1.0f);
				if (Button("Clear")) {
					Host::ClearFolder();
					loadAt = 0;
				}
				Tip("Stops searching this folder. Plugins already in the chain keep working until the game restarts.");
			}
			SameLine(0.0f, -1.0f);
			AlignTextToFramePadding();
			if (folder.empty()) {
				TextDisabled("Extra plugin folder: none");
			} else {
				TextDisabled("Extra plugin folder: %s", folder.c_str());
			}

			if (!view.problems.empty()) {
				TextColored(kRed, "%zu plugin file(s) could not be read.", view.problems.size());
				std::string all;
				for (const auto& problem : view.problems) {
					all += problem + "\n";
				}
				Tip(all.c_str());
			}

			Separator();
			TextDisabled("%s", statusText.c_str());
			bool anyOpen = false;
			for (const auto& slot : view.slots) {
				anyOpen = anyOpen || slot.editorOpen;
			}
			if (anyOpen) {
				TextDisabled("If the sound stops while a plugin window has focus, set bAlwaysActive=1 under [General] in Skyrim.ini.");
			}
		}

		void __stdcall RenderWindow()
		{
			const auto  version = Plugin::VERSION;
			const auto  key = KeyName(Settings::GetSingleton().toggleKey);
			const auto  title = std::format("Audio Plugin Host v{}.{}.{} ({} to close)###AudioPluginHost", version.major(), version.minor(), version.patch(), key);
			const auto* display = GetIO();
			const float scale = std::clamp(windowScale, 0.5f, 3.0f);
			SetNextWindowSize(ImVec2((std::min)(display->DisplaySize.x - 80.0f, 760.0f * scale), 0.0f), ImGuiCond_Always);
			SetNextWindowPos(ImVec2(40.0f, display->DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.0f, 0.5f));
			PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 8.0f * scale));
			PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
			PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * scale);
			PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
			PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * scale, 3.0f * scale));
			PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 5.0f * scale));
			PushStyleColor(ImGuiCol_Text, IM_COL32(204, 204, 199, 255));
			PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(150, 150, 145, 255));
			PushStyleColor(ImGuiCol_WindowBg, IM_COL32(5, 5, 5, 240));
			PushStyleColor(ImGuiCol_TitleBg, IM_COL32(0, 0, 0, 204));
			PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(5, 5, 5, 204));
			PushStyleColor(ImGuiCol_CheckMark, IM_COL32(199, 166, 92, 255));
			if (Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
				if (fontScale <= 0.0f) {
					SetWindowFontScale(1.0f);
					fontScale = (std::min)(1.0f, 18.0f / GetFontSize());
				}
				SetWindowFontScale(fontScale * scale);
				Render();
			}
			End();
			PopStyleColor(6);
			PopStyleVar(7);
		}

		void __stdcall RenderPage()
		{
			auto& settings = Settings::GetSingleton();
			if (Button("Open plugin host")) {
				window->IsOpen = true;
			}

			const bool waiting = waitingForKey.load();
			Text("Open key");
			SameLine();
			const auto label = std::format("{}###OpenKey", waiting ? std::string("Press a key...") : KeyName(settings.toggleKey));
			if (Button(label.c_str())) {
				waitingForKey = true;
			}
			if (waiting) {
				SameLine();
				TextDisabled("Esc cancels.");
			}

			if (Zoom(windowScale, 100.0f)) {
				SaveSettings();
			}
			Tip("Size of the plugin host window. 200% suits 4K screens.");

			if (Checkbox("Enabled", &enabled)) {
				Rack::SetEnabled(enabled);
				SaveSettings();
			}
		}

		bool __stdcall OnInput(RE::InputEvent* a_event)
		{
			if (!a_event) {
				return false;
			}
			if (a_event->device == RE::INPUT_DEVICE::kMouse) {
				const auto* press = a_event->AsButtonEvent();
				return Host::PointerOnPlugin() && !(press && press->IsUp());
			}
			if (a_event->device != RE::INPUT_DEVICE::kKeyboard) {
				return false;
			}
			const auto* button = a_event->AsButtonEvent();
			if (!button || !button->IsDown()) {
				return false;
			}
			auto&      settings = Settings::GetSingleton();
			const auto code = button->GetIDCode();
			if (waitingForKey.exchange(false)) {
				const auto key = ScanToKey(code);
				if (code != RE::BSWin32KeyboardDevice::Key::kEscape && key != 0) {
					settings.toggleKey = key;
					SaveSettings();
				}
				return true;
			}
			if (code != KeyToScan(settings.toggleKey)) {
				return false;
			}
			window->IsOpen = !window->IsOpen.load();
			return true;
		}
	}

	namespace
	{
		void __stdcall OnHud()
		{
			if (meterDrawn && GetTickCount64() - meterDrawn > 500) {
				meterDrawn = 0;
				Rack::SetMetering(false);
			}
		}
	}

	void Register()
	{
		const auto& settings = Settings::GetSingleton();
		enabled = settings.enabled;
		editorsOnTop = settings.editorsOnTop;
		stereoMaster = settings.stereoMaster;
		windowScale = std::clamp(settings.windowScale, 0.5f, 3.0f);
		if (!SKSEMenuFramework::IsInstalled() || !GetMenuFrameworkModule()) {
			logger::error("SKSE Menu Framework is not installed. The saved plugins keep running, without a window.");
			return;
		}
		SKSEMenuFramework::SetSection("Audio Plugin Host");
		SKSEMenuFramework::AddSectionItem("Plugins", RenderPage);
		window = SKSEMenuFramework::AddWindow(RenderWindow, true);
		SKSEMenuFramework::AddInputEvent(OnInput);
		SKSEMenuFramework::AddHudElement(OnHud);
		logger::info("Registered with SKSE Menu Framework.");
	}
}
