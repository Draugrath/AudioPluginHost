#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Vst
{
	inline constexpr std::uint32_t kMaxChannels = 8;
	inline constexpr std::int32_t  kMaxBlock = 1024;

	struct PluginInfo
	{
		std::string path;
		std::string uid;
		std::string name;
		std::string vendor;

		bool operator==(const PluginInfo&) const = default;
	};

	std::vector<PluginInfo> Scan(const std::vector<std::filesystem::path>& a_folders, std::vector<std::string>* a_problems = nullptr);

	class Slot
	{
	public:
		static std::unique_ptr<Slot> Create(const PluginInfo& a_info, double a_rate, std::uint32_t a_channels, std::string& a_error);
		~Slot();

		Slot(const Slot&) = delete;
		Slot& operator=(const Slot&) = delete;

		[[nodiscard]] const PluginInfo& Info() const;
		[[nodiscard]] std::string       Layout() const;
		[[nodiscard]] std::int32_t      Latency() const;
		[[nodiscard]] bool              HasEditor() const;
		[[nodiscard]] bool              EditorOpen() const;
		[[nodiscard]] bool              Crashed() const;

		bool OpenEditor(bool a_onTop);
		void CloseEditor();
		void SetEditorOnTop(bool a_onTop);
		void Idle();

		[[nodiscard]] std::vector<char> SaveState() const;
		bool                            LoadState(const std::vector<char>& a_state);

		std::atomic<bool> enabled{ true };

		std::atomic<std::uint64_t> spentTicks{ 0 };

		void Process(float* a_samples, std::uint32_t a_frames, std::uint32_t a_channels);
		[[nodiscard]] bool Busy() const;

		struct Impl;

	private:
		Slot() = default;
		std::unique_ptr<Impl> impl;
	};
}
