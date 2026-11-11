#include "Rack.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <future>
#include <mutex>
#include <thread>

namespace Rack
{
	namespace
	{
		struct Snapshot
		{
			std::vector<Vst::Slot*> slots;
			bool                    stereo{ false };
		};
		std::atomic<Snapshot*> current{ nullptr };
		std::atomic<Snapshot*> reading{ nullptr };

		struct Entry
		{
			Saved                      saved;
			Vst::PluginInfo            info;
			std::unique_ptr<Vst::Slot> slot;
			std::string                error;
			float                      cpu{ 0.0f };
		};

		Config                       config;
		std::vector<Entry>&          entries = *new std::vector<Entry>;
		std::vector<Vst::PluginInfo> available;
		std::vector<std::string>     problems;
		std::string                  message;
		bool                         editorsShown = false;

		std::mutex                        viewLock;
		std::shared_ptr<const View>       view = std::make_shared<const View>();
		std::mutex                        taskLock;
		std::deque<std::function<void()>> tasks;
		HANDLE                            wake = nullptr;
		std::thread*                      worker = nullptr;
		std::atomic<bool>                 started{ false };
		std::atomic<bool>                 quit{ false };
		std::atomic<bool>                 scanning{ false };

		std::atomic<bool>          metering{ false };
		std::atomic<float>         peaks[Vst::kMaxChannels]{};
		std::atomic<std::uint32_t> meterChannels{ 0 };
		std::atomic<std::uint64_t> framesDone{ 0 };
		std::atomic<std::uint64_t> restAfter{ 480000 };
		std::uint64_t              silentFrames = 0;
		constexpr std::uint32_t    kStereoBlock = 4096;
		float                      stereoBuffer[kStereoBlock * 2]{};

		std::uint64_t cpuFrames = 0;
		ULONGLONG     cpuAt = 0;
		double        ticksPerSecond = 0.0;

		template <class... Args>
		void Log(bool a_warning, std::format_string<Args...> a_format, Args&&... a_args)
		{
			if (config.log) {
				config.log(a_warning, std::format(a_format, std::forward<Args>(a_args)...));
			}
		}

		bool Stereo();

		void MeasureCpu()
		{
			const auto now = GetTickCount64();
			if (now - cpuAt < 1000) {
				return;
			}
			cpuAt = now;
			if (ticksPerSecond <= 0.0) {
				LARGE_INTEGER frequency{};
				QueryPerformanceFrequency(&frequency);
				ticksPerSecond = static_cast<double>(frequency.QuadPart);
			}
			const auto   frames = framesDone.load(std::memory_order_relaxed);
			const double audio = static_cast<double>(frames - cpuFrames) / config.rate;
			cpuFrames = frames;
			for (auto& entry : entries) {
				if (!entry.slot) {
					continue;
				}
				const auto ticks = entry.slot->spentTicks.exchange(0, std::memory_order_relaxed);
				const double percent = audio > 0.0 ? static_cast<double>(ticks) / ticksPerSecond / audio * 100.0 : 0.0;
				entry.cpu = static_cast<float>(std::round(percent * 10.0) / 10.0);
			}
		}

		void RefreshView()
		{
			MeasureCpu();
			View next;
			for (const auto& entry : entries) {
				SlotView slot;
				slot.name = entry.info.name;
				slot.vendor = entry.info.vendor;
				slot.on = entry.saved.on;
				slot.error = entry.error;
				if (entry.slot) {
					slot.loaded = true;
					slot.layout = entry.slot->Layout();
					slot.latency = entry.slot->Latency();
					slot.cpu = entry.cpu;
					slot.hasEditor = entry.slot->HasEditor();
					slot.editorOpen = entry.slot->EditorOpen();
					slot.crashed = entry.slot->Crashed();
					if (slot.on && !slot.crashed && config.enabled) {
						next.latency += slot.latency;
					}
				}
				next.slots.push_back(std::move(slot));
			}
			next.available = available;
			next.problems = problems;
			next.message = message;
			next.rate = config.rate;
			next.scanning = scanning.load();
			next.started = true;
			next.stereo = Stereo();
			{
				std::scoped_lock held(taskLock);
				next.busy = !tasks.empty();
			}
			std::scoped_lock lock(viewLock);
			if (!(*view == next)) {
				view = std::make_shared<const View>(std::move(next));
			}
		}

		bool Stereo() { return config.stereo && config.channels == 6; }
		std::uint32_t SlotChannels() { return Stereo() ? 2u : config.channels; }

		void ApplyOn()
		{
			for (auto& entry : entries) {
				if (entry.slot) {
					entry.slot->enabled = config.enabled && entry.saved.on;
				}
			}
		}

		bool Publish()
		{
			auto* next = new Snapshot;
			next->stereo = Stereo();
			for (auto& entry : entries) {
				if (entry.slot) {
					next->slots.push_back(entry.slot.get());
				}
			}
			Snapshot* old = current.exchange(next);
			if (!old) {
				return true;
			}
			for (int waited = 0; reading.load() == old; ++waited) {
				if (waited > 2000) {
					Log(true, "The audio thread did not let go of the old plugin list; leaving it in memory.");
					return false;
				}
				Sleep(1);
			}
			delete old;
			return true;
		}

		void Free(std::unique_ptr<Vst::Slot> a_slot, bool a_safe)
		{
			if (!a_slot) {
				return;
			}
			a_slot->CloseEditor();
			if (!a_safe) {
				(void)a_slot.release();
			}
		}

		Vst::PluginInfo Lookup(const Saved& a_saved)
		{
			for (const auto& plugin : available) {
				if (plugin.uid == a_saved.uid && _stricmp(plugin.path.c_str(), a_saved.path.c_str()) == 0) {
					return plugin;
				}
			}
			for (const auto& plugin : available) {
				if (plugin.uid == a_saved.uid) {
					return plugin;
				}
			}
			return { a_saved.path, a_saved.uid, std::filesystem::u8path(a_saved.path).stem().string(), "" };
		}

		void Load(Entry& a_entry)
		{
			a_entry.info = Lookup(a_entry.saved);
			std::string error;
			a_entry.slot = Vst::Slot::Create(a_entry.info, config.rate, SlotChannels(), error);
			if (!a_entry.slot) {
				a_entry.error = error.empty() ? "The plugin could not be loaded." : error;
				Log(true, "{}: {}", a_entry.info.name, a_entry.error);
				return;
			}
			a_entry.error.clear();
			a_entry.saved.path = a_entry.info.path;
			if (!a_entry.saved.state.empty() && !a_entry.slot->LoadState(a_entry.saved.state)) {
				Log(true, "{}: its saved settings did not load.", a_entry.info.name);
			}
			Log(false, "Loaded {} ({}, {}, latency {} samples).", a_entry.info.name, a_entry.info.vendor, a_entry.slot->Layout(), a_entry.slot->Latency());
		}

		void Scan()
		{
			scanning = true;
			RefreshView();
			problems.clear();
			available = Vst::Scan(config.folders, &problems);
			scanning = false;
			Log(false, "Found {} VST3 effects.", available.size());
			for (const auto& problem : problems) {
				Log(true, "{}", problem);
			}
		}

		std::vector<Saved> Collect(bool a_states)
		{
			std::vector<Saved> slots;
			for (auto& entry : entries) {
				if (a_states && entry.slot) {
					entry.saved.state = entry.slot->SaveState();
				}
				slots.push_back(entry.saved);
				if (!a_states) {
					slots.back().state.clear();
				}
			}
			return slots;
		}

		void ShowEditors()
		{
			bool open = false;
			for (const auto& entry : entries) {
				open = open || (entry.slot && entry.slot->EditorOpen());
			}
			if (open != editorsShown) {
				editorsShown = open;
				if (config.onEditors) {
					config.onEditors(open);
				}
			}
		}

		void Run()
		{
			OleInitialize(nullptr);
			Scan();
			auto saved = std::move(config.slots);
			config.slots.clear();
			for (auto& slot : saved) {
				Entry entry;
				entry.saved = std::move(slot);
				Load(entry);
				entries.push_back(std::move(entry));
			}
			ApplyOn();
			Publish();
			RefreshView();
			while (!quit.load()) {
				MsgWaitForMultipleObjectsEx(1, &wake, editorsShown ? 50 : 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
				MSG msg;
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				}
				std::deque<std::function<void()>> now;
				{
					std::scoped_lock lock(taskLock);
					now.swap(tasks);
				}
				for (auto& task : now) {
					task();
				}
				for (auto& entry : entries) {
					if (entry.slot) {
						entry.slot->Idle();
					}
				}
				ShowEditors();
				RefreshView();
			}
			const bool safe = [] {
				auto* none = new Snapshot;
				Snapshot* old = current.exchange(none);
				for (int waited = 0; old && reading.load() == old; ++waited) {
					if (waited > 2000) {
						return false;
					}
					Sleep(1);
				}
				delete old;
				return true;
			}();
			for (auto& entry : entries) {
				Free(std::move(entry.slot), safe);
			}
			entries.clear();
			ShowEditors();
			OleUninitialize();
		}

		void Post(std::function<void()> a_task)
		{
			if (!started.load()) {
				return;
			}
			{
				std::scoped_lock lock(taskLock);
				tasks.push_back(std::move(a_task));
			}
			SetEvent(wake);
		}
	}

	void Start(Config a_config)
	{
		if (started.exchange(true)) {
			return;
		}
		config = std::move(a_config);
		quit = false;
		restAfter = static_cast<std::uint64_t>(config.rate * 10.0);
		wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		worker = new std::thread(Run);
	}

	void Stop()
	{
		if (!started.load()) {
			return;
		}
		quit = true;
		SetEvent(wake);
		if (worker) {
			worker->join();
			delete worker;
			worker = nullptr;
		}
		started = false;
		CloseHandle(wake);
		wake = nullptr;
		std::scoped_lock lock(taskLock);
		tasks.clear();
		available.clear();
		problems.clear();
		message.clear();
		std::scoped_lock held(viewLock);
		view = std::make_shared<const View>();
	}

	void Sync()
	{
		auto done = std::make_shared<std::promise<void>>();
		auto future = done->get_future();
		Post([done] {
			ShowEditors();
			RefreshView();
			done->set_value();
		});
		if (started.load()) {
			future.wait_for(std::chrono::seconds(60));
		}
	}

	namespace
	{
		void RunSlot(Vst::Slot* a_slot, float* a_samples, std::uint32_t a_frames, std::uint32_t a_channels)
		{
			LARGE_INTEGER before{}, after{};
			QueryPerformanceCounter(&before);
			a_slot->Process(a_samples, a_frames, a_channels);
			QueryPerformanceCounter(&after);
			a_slot->spentTicks.fetch_add(static_cast<std::uint64_t>(after.QuadPart - before.QuadPart), std::memory_order_relaxed);
		}

		void RaisePeaks(const float* a_most, std::uint32_t a_channels)
		{
			meterChannels.store(a_channels, std::memory_order_relaxed);
			for (std::uint32_t c = 0; c < a_channels; ++c) {
				if (a_most[c] > peaks[c].load(std::memory_order_relaxed)) {
					peaks[c].store(a_most[c], std::memory_order_relaxed);
				}
			}
		}

		bool ProcessStereo(const Snapshot& a_snapshot, float* a_samples, std::uint32_t a_frames, bool a_silent)
		{
			bool busy = false;
			for (auto* slot : a_snapshot.slots) {
				busy = busy || slot->Busy();
			}
			const bool measure = metering.load(std::memory_order_relaxed);
			if (!busy && !measure) {
				return false;
			}
			float most[2]{};
			for (std::uint32_t done = 0; done < a_frames;) {
				const std::uint32_t frames = (std::min)(a_frames - done, kStereoBlock);
				float*              block = a_samples + static_cast<std::size_t>(done) * 6;
				float*              stereo = stereoBuffer;
				if (a_silent) {
					std::memset(stereo, 0, static_cast<std::size_t>(frames) * 2 * sizeof(float));
				} else {
					for (std::uint32_t i = 0; i < frames; ++i) {
						const float* in = block + static_cast<std::size_t>(i) * 6;
						stereo[i * 2] = in[0] * kFoldFront + in[2] * kFoldCenter + in[3] * kFoldBass + in[4] * kFoldRearSame + in[5] * kFoldRearOther;
						stereo[i * 2 + 1] = in[1] * kFoldFront + in[2] * kFoldCenter + in[3] * kFoldBass + in[5] * kFoldRearSame + in[4] * kFoldRearOther;
					}
				}
				if (busy) {
					for (auto* slot : a_snapshot.slots) {
						if (slot->Busy()) {
							RunSlot(slot, stereo, frames, 2);
						}
					}
					for (std::uint32_t i = 0; i < frames; ++i) {
						float* out = block + static_cast<std::size_t>(i) * 6;
						out[0] = stereo[i * 2] * kUnfold;
						out[1] = stereo[i * 2 + 1] * kUnfold;
						out[2] = out[3] = out[4] = out[5] = 0.0f;
					}
				}
				if (measure) {
					for (std::uint32_t i = 0; i < frames * 2; i += 2) {
						most[0] = (std::max)(most[0], std::fabs(stereo[i]));
						most[1] = (std::max)(most[1], std::fabs(stereo[i + 1]));
					}
				}
				done += frames;
			}
			if (measure) {
				RaisePeaks(most, 2);
			}
			return busy;
		}
	}

	bool Process(float* a_samples, std::uint32_t a_frames, std::uint32_t a_channels, bool a_silent)
	{
		Snapshot* snapshot = nullptr;
		do {
			snapshot = current.load();
			reading.store(snapshot);
		} while (snapshot != current.load());
		bool       ran = false;
		const bool stereo = snapshot && snapshot->stereo && a_channels == 6;
		silentFrames = a_silent ? silentFrames + a_frames : 0;
		if (silentFrames > restAfter.load(std::memory_order_relaxed)) {
			snapshot = nullptr;
		}
		if (stereo && snapshot) {
			ran = ProcessStereo(*snapshot, a_samples, a_frames, a_silent);
		} else if (snapshot) {
			for (auto* slot : snapshot->slots) {
				if (!slot->Busy()) {
					continue;
				}
				if (!ran && a_silent) {
					std::memset(a_samples, 0, static_cast<std::size_t>(a_frames) * a_channels * sizeof(float));
				}
				ran = true;
				RunSlot(slot, a_samples, a_frames, a_channels);
			}
		}
		reading.store(nullptr);
		framesDone.fetch_add(a_frames, std::memory_order_relaxed);
		if (!stereo && metering.load(std::memory_order_relaxed)) {
			const std::uint32_t channels = (std::min)(a_channels, Vst::kMaxChannels);
			float               most[Vst::kMaxChannels]{};
			if (ran || !a_silent) {
				for (std::uint32_t i = 0; i < a_frames; ++i) {
					const float* frame = a_samples + static_cast<std::size_t>(i) * a_channels;
					for (std::uint32_t c = 0; c < channels; ++c) {
						most[c] = (std::max)(most[c], std::fabs(frame[c]));
					}
				}
			}
			RaisePeaks(most, channels);
		}
		return ran;
	}

	void SetMetering(bool a_on) { metering = a_on; }

	std::uint32_t ReadMeter(float* a_peaks)
	{
		for (std::uint32_t c = 0; c < Vst::kMaxChannels; ++c) {
			a_peaks[c] = peaks[c].exchange(0.0f, std::memory_order_relaxed);
		}
		return meterChannels.load(std::memory_order_relaxed);
	}

	View Read()
	{
		return *Peek();
	}

	std::shared_ptr<const View> Peek()
	{
		std::scoped_lock lock(viewLock);
		return view;
	}

	void Add(const Vst::PluginInfo& a_plugin)
	{
		Post([a_plugin] {
			Entry entry;
			entry.saved = { true, a_plugin.uid, a_plugin.path, {} };
			entry.info = a_plugin;
			std::string error;
			entry.slot = Vst::Slot::Create(a_plugin, config.rate, SlotChannels(), error);
			if (!entry.slot) {
				message = std::format("{} did not load: {}", a_plugin.name, error);
				Log(true, "{}", message);
				return;
			}
			message.clear();
			Log(false, "Added {} ({}, latency {} samples).", a_plugin.name, entry.slot->Layout(), entry.slot->Latency());
			entries.push_back(std::move(entry));
			ApplyOn();
			Publish();
		});
	}

	void Remove(std::size_t a_slot)
	{
		Post([a_slot] {
			if (a_slot >= entries.size()) {
				return;
			}
			auto slot = std::move(entries[a_slot].slot);
			entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(a_slot));
			Free(std::move(slot), Publish());
		});
	}

	void MoveTo(std::size_t a_from, std::size_t a_to)
	{
		Post([a_from, a_to] {
			if (a_from >= entries.size() || a_to >= entries.size() || a_from == a_to) {
				return;
			}
			const auto begin = entries.begin();
			if (a_from < a_to) {
				std::rotate(begin + static_cast<std::ptrdiff_t>(a_from), begin + static_cast<std::ptrdiff_t>(a_from) + 1, begin + static_cast<std::ptrdiff_t>(a_to) + 1);
			} else {
				std::rotate(begin + static_cast<std::ptrdiff_t>(a_to), begin + static_cast<std::ptrdiff_t>(a_from), begin + static_cast<std::ptrdiff_t>(a_from) + 1);
			}
			Publish();
		});
	}

	void SetOn(std::size_t a_slot, bool a_on)
	{
		Post([a_slot, a_on] {
			if (a_slot < entries.size()) {
				entries[a_slot].saved.on = a_on;
				ApplyOn();
			}
		});
	}

	void ToggleEditor(std::size_t a_slot)
	{
		Post([a_slot] {
			if (a_slot >= entries.size() || !entries[a_slot].slot) {
				return;
			}
			auto& slot = *entries[a_slot].slot;
			if (slot.EditorOpen()) {
				slot.CloseEditor();
			} else if (!slot.OpenEditor(config.editorsOnTop)) {
				message = std::format("{} has no window this host can show.", entries[a_slot].info.name);
			}
			ShowEditors();
		});
	}

	void SetEnabled(bool a_enabled)
	{
		Post([a_enabled] {
			config.enabled = a_enabled;
			ApplyOn();
		});
	}

	void SetEditorsOnTop(bool a_onTop)
	{
		Post([a_onTop] {
			config.editorsOnTop = a_onTop;
			for (auto& entry : entries) {
				if (entry.slot) {
					entry.slot->SetEditorOnTop(a_onTop);
				}
			}
		});
	}

	void SetStereo(bool a_stereo)
	{
		Post([a_stereo] {
			const bool was = Stereo();
			config.stereo = a_stereo;
			if (Stereo() == was) {
				return;
			}
			std::vector<std::unique_ptr<Vst::Slot>> old;
			std::vector<Entry*>                     reopen;
			for (auto& entry : entries) {
				if (!entry.slot) {
					continue;
				}
				entry.saved.state = entry.slot->SaveState();
				const bool  open = entry.slot->EditorOpen();
				std::string error;
				auto        fresh = Vst::Slot::Create(entry.info, config.rate, SlotChannels(), error);
				old.push_back(std::move(entry.slot));
				if (!fresh) {
					entry.error = error.empty() ? "The plugin could not be loaded." : error;
					Log(true, "{}: {}", entry.info.name, entry.error);
					continue;
				}
				fresh->LoadState(entry.saved.state);
				entry.slot = std::move(fresh);
				if (open) {
					reopen.push_back(&entry);
				}
			}
			ApplyOn();
			const bool safe = Publish();
			for (auto& slot : old) {
				Free(std::move(slot), safe);
			}
			for (auto* entry : reopen) {
				entry->slot->OpenEditor(config.editorsOnTop);
			}
			Log(false, "Stereo master {}.", Stereo() ? "on: the plugins process the engine's stereo mix" : "off: the plugins process the game's channels");
		});
	}

	void SetFolders(std::vector<std::filesystem::path> a_folders)
	{
		Post([folders = std::move(a_folders)]() mutable { config.folders = std::move(folders); });
	}

	void Rescan()
	{
		Post([] {
			Scan();
			for (auto& entry : entries) {
				if (!entry.slot) {
					Load(entry);
				}
			}
			ApplyOn();
			Publish();
		});
	}

	void Save(bool a_states)
	{
		Post([a_states] {
			if (config.onSave) {
				config.onSave(Collect(a_states), a_states);
			}
			if (a_states) {
				message = "Saved.";
			}
		});
	}
}
