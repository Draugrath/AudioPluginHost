#pragma once

#include "Vst3Host.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Rack
{
	inline constexpr float kFoldFront = 0.294545442f;
	inline constexpr float kFoldCenter = 0.208181813f;
	inline constexpr float kFoldBass = 0.090909094f;
	inline constexpr float kFoldRearSame = 0.251818180f;
	inline constexpr float kFoldRearOther = 0.154545456f;
	inline constexpr float kUnfold = 1.0f / kFoldFront;

	struct Saved
	{
		bool              on{ true };
		std::string       uid;
		std::string       path;
		std::vector<char> state;
	};

	struct Config
	{
		std::vector<std::filesystem::path> folders;
		double                             rate{ 48000.0 };
		std::uint32_t                      channels{ 6 };
		bool                               enabled{ true };
		bool                               editorsOnTop{ true };
		bool                               stereo{ false };
		std::vector<Saved>                 slots;

		std::function<void(bool a_warning, const std::string& a_text)>     log;
		std::function<void(bool a_open)>                                   onEditors;
		std::function<void()>                                              onIdle;
		std::function<void(const std::vector<Saved>& a_slots, bool a_states)> onSave;
	};

	struct SlotView
	{
		std::string  name;
		std::string  vendor;
		std::string  layout;
		std::string  error;
		std::int32_t latency{ 0 };
		float        cpu{ 0.0f };
		bool         on{ true };
		bool         loaded{ false };
		bool         hasEditor{ false };
		bool         editorOpen{ false };
		bool         crashed{ false };

		bool operator==(const SlotView&) const = default;
	};

	struct View
	{
		std::vector<SlotView>        slots;
		std::vector<Vst::PluginInfo> available;
		std::vector<std::string>     problems;
		std::string                  message;
		std::int32_t                 latency{ 0 };
		double                       rate{ 0.0 };
		bool                         scanning{ false };
		bool                         busy{ false };
		bool                         started{ false };
		bool                         stereo{ false };

		bool operator==(const View&) const = default;
	};

	void Start(Config a_config);
	void Stop();
	void Sync();

	bool Process(float* a_samples, std::uint32_t a_frames, std::uint32_t a_channels, bool a_silent);

	void          SetMetering(bool a_on);
	std::uint32_t ReadMeter(float* a_peaks);

	View Read();
	std::shared_ptr<const View> Peek();

	void Add(const Vst::PluginInfo& a_plugin);
	void Remove(std::size_t a_slot);
	void MoveTo(std::size_t a_from, std::size_t a_to);
	void SetOn(std::size_t a_slot, bool a_on);
	void ToggleEditor(std::size_t a_slot);
	void SetEnabled(bool a_enabled);
	void SetEditorsOnTop(bool a_onTop);
	void SetStereo(bool a_stereo);
	void SetFolders(std::vector<std::filesystem::path> a_folders);
	void Rescan();
	void Save(bool a_states);
}
