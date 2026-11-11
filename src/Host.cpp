#include "Host.h"
#include "AudioChain.h"
#include "CursorGuard.h"
#include "Rack.h"
#include "Settings.h"

#include <RE/B/BSAudioManager.h>
#include <RE/B/BSInputDeviceManager.h>
#include <RE/B/BSWin32MouseDevice.h>
#include <RE/B/BSXAudio2Audio.h>
#include <RE/I/IXAudio2Voice.h>
#include <RE/M/Main.h>
#include <RE/X/XAPO.h>
#include <RE/X/XAudio2.h>

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <xmmintrin.h>

#include <mutex>
#include <thread>

namespace
{
	constexpr GUID          kIUnknown{ 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
	constexpr GUID          kIXAPO{ 0xA90BC001, 0xE897, 0xE897, { 0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x00 } };
	constexpr std::int32_t  kOk = 0;
	constexpr std::int32_t  kNoInterface = static_cast<std::int32_t>(0x80004002);
	constexpr std::int32_t  kFormatUnsupported = static_cast<std::int32_t>(0x88970001);
	constexpr std::uint32_t kFlags = 0x1 | 0x2 | 0x4 | 0x8 | 0x10;
	constexpr std::int32_t  kChainOrder = 10;

	bool IsFloat(const RE::WAVEFORMATEX* a_format)
	{
		const auto& format = reinterpret_cast<const WAVEFORMATEX&>(*a_format);
		return format.wBitsPerSample == 32 && (format.wFormatTag == 3 || format.wFormatTag == 0xFFFE);
	}

	class Effect final : public RE::IXAPO
	{
	public:
		std::int32_t QueryInterface(const REX::W32::GUID& a_iid, void** a_out) override
		{
			if (!a_out) {
				return kNoInterface;
			}
			if (std::memcmp(&a_iid, &kIUnknown, sizeof(GUID)) == 0 || std::memcmp(&a_iid, &kIXAPO, sizeof(GUID)) == 0) {
				*a_out = this;
				AddRef();
				return kOk;
			}
			*a_out = nullptr;
			return kNoInterface;
		}

		std::uint32_t AddRef() override { return ++references; }
		std::uint32_t Release() override { return --references; }

		std::int32_t GetRegistrationProperties(RE::XAPO_REGISTRATION_PROPERTIES** a_properties) override
		{
			auto* properties = static_cast<RE::XAPO_REGISTRATION_PROPERTIES*>(CoTaskMemAlloc(sizeof(RE::XAPO_REGISTRATION_PROPERTIES)));
			if (!properties) {
				return static_cast<std::int32_t>(0x8007000E);
			}
			std::memset(properties, 0, sizeof(*properties));
			wcscpy_s(properties->friendlyName, L"Audio Plugin Host");
			properties->majorVersion = 1;
			properties->flags = kFlags;
			properties->minInputBufferCount = properties->maxInputBufferCount = 1;
			properties->minOutputBufferCount = properties->maxOutputBufferCount = 1;
			*a_properties = properties;
			return kOk;
		}

		std::int32_t IsInputFormatSupported(const RE::WAVEFORMATEX*, const RE::WAVEFORMATEX* a_requested, RE::WAVEFORMATEX** a_supported) override
		{
			if (a_supported) {
				*a_supported = nullptr;
			}
			return IsFloat(a_requested) ? kOk : kFormatUnsupported;
		}

		std::int32_t IsOutputFormatSupported(const RE::WAVEFORMATEX*, const RE::WAVEFORMATEX* a_requested, RE::WAVEFORMATEX** a_supported) override
		{
			if (a_supported) {
				*a_supported = nullptr;
			}
			return IsFloat(a_requested) ? kOk : kFormatUnsupported;
		}

		std::int32_t Initialize(const void*, std::uint32_t) override { return kOk; }
		void         Reset() override {}

		std::int32_t LockForProcess(std::uint32_t, const RE::XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* a_input, std::uint32_t, const RE::XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS*) override
		{
			const auto& format = reinterpret_cast<const WAVEFORMATEX&>(*a_input->format);
			channels = format.nChannels;
			sampleRate.store(format.nSamplesPerSec);
			return kOk;
		}

		void UnlockForProcess() override {}

		void Process(std::uint32_t, const RE::XAPO_PROCESS_BUFFER_PARAMETERS* a_input, std::uint32_t, RE::XAPO_PROCESS_BUFFER_PARAMETERS* a_output, BOOL) override
		{
			LARGE_INTEGER started{};
			QueryPerformanceCounter(&started);
			const std::uint32_t frames = a_input->validFrameCount;
			a_output->validFrameCount = frames;
			a_output->bufferFlags = a_input->bufferFlags;
			if (a_input->buffer != a_output->buffer) {
				std::memcpy(a_output->buffer, a_input->buffer, static_cast<std::size_t>(frames) * channels * sizeof(float));
			}
			const bool silent = a_input->bufferFlags == RE::XAPO_BUFFER_FLAGS::XAPO_BUFFER_SILENT;
			const auto state = _mm_getcsr();
			_mm_setcsr(state | 0x8040);
			if (Rack::Process(static_cast<float*>(a_output->buffer), frames, channels, silent)) {
				a_output->bufferFlags = RE::XAPO_BUFFER_FLAGS::XAPO_BUFFER_VALID;
			}
			_mm_setcsr(state);
			LARGE_INTEGER ended{};
			QueryPerformanceCounter(&ended);
			const auto spent = static_cast<std::uint64_t>(ended.QuadPart - started.QuadPart);
			spentTicks.fetch_add(spent, std::memory_order_relaxed);
			blockFrames.fetch_add(frames, std::memory_order_relaxed);
			if (spent > mostTicks.load(std::memory_order_relaxed)) {
				mostTicks.store(spent, std::memory_order_relaxed);
				mostFrames.store(frames, std::memory_order_relaxed);
			}
		}

		std::uint32_t CalcInputFrames(std::uint32_t a_frames) override { return a_frames; }
		std::uint32_t CalcOutputFrames(std::uint32_t a_frames) override { return a_frames; }

		std::string LoadText()
		{
			static const LARGE_INTEGER frequency = [] {
				LARGE_INTEGER value{};
				QueryPerformanceFrequency(&value);
				return value;
			}();
			const auto spent = spentTicks.exchange(0, std::memory_order_relaxed);
			const auto total = blockFrames.exchange(0, std::memory_order_relaxed);
			const auto most = mostTicks.exchange(0, std::memory_order_relaxed);
			const auto mostAt = mostFrames.exchange(0, std::memory_order_relaxed);
			const auto rate = sampleRate.load();
			if (total && rate) {
				const double audio = static_cast<double>(total) / rate;
				const double used = static_cast<double>(spent) / static_cast<double>(frequency.QuadPart);
				const double peak = mostAt ? static_cast<double>(most) / static_cast<double>(frequency.QuadPart) / (static_cast<double>(mostAt) / rate) : 0.0;
				shown = std::format("Plugins use {:.1f}% of the audio time (busiest block {:.0f}%).", used / audio * 100.0, peak * 100.0);
			}
			return shown;
		}

	private:
		std::atomic<std::uint32_t> references{ 1 };
		std::uint32_t              channels{ 2 };
		std::atomic<std::uint32_t> sampleRate{ 0 };
		std::atomic<std::uint64_t> spentTicks{ 0 };
		std::atomic<std::uint64_t> blockFrames{ 0 };
		std::atomic<std::uint64_t> mostTicks{ 0 };
		std::atomic<std::uint32_t> mostFrames{ 0 };
		std::string                shown;
	};

	Effect&     effect = *new Effect;
	std::mutex  settingsLock;
	std::atomic<bool> stereoAvailable{ false };
	std::string       stereoNote = "The game's sound is not attached yet.";

	std::uint32_t DeviceChannels()
	{
		const bool           started = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
		std::uint32_t        channels = 0;
		IMMDeviceEnumerator* enumerator = nullptr;
		IMMDevice*           device = nullptr;
		IAudioClient*        client = nullptr;
		WAVEFORMATEX*        format = nullptr;
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
			SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
			SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) &&
			SUCCEEDED(client->GetMixFormat(&format)) && format) {
			channels = format->nChannels;
			CoTaskMemFree(format);
		}
		if (client) {
			client->Release();
		}
		if (device) {
			device->Release();
		}
		if (enumerator) {
			enumerator->Release();
		}
		if (started) {
			CoUninitialize();
		}
		return channels;
	}

	std::mutex  statusLock;
	std::string status = "Not attached.";

	void SetStatus(std::string a_text, bool a_error)
	{
		if (a_error) {
			logger::error("{}", a_text);
		} else {
			logger::info("{}", a_text);
		}
		std::scoped_lock lock(statusLock);
		status = std::move(a_text);
	}

	std::vector<std::filesystem::path> Folders(const std::string& a_extra)
	{
		std::vector<std::filesystem::path> folders;
		PWSTR                              common = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesCommon, 0, nullptr, &common))) {
			folders.emplace_back(std::filesystem::path(common) / L"VST3");
		}
		CoTaskMemFree(common);
		folders.emplace_back(std::filesystem::path(Settings::kFolder) / "VST3");
		if (!a_extra.empty()) {
			folders.emplace_back(std::filesystem::u8path(a_extra));
		}
		return folders;
	}

	void SaveSlots(const std::vector<Rack::Saved>& a_slots)
	{
		std::scoped_lock lock(settingsLock);
		auto&            settings = Settings::GetSingleton();
		std::error_code  error;
		std::filesystem::create_directories(Settings::kFolder, error);
		settings.slots.clear();
		for (std::size_t i = 0; i < a_slots.size(); ++i) {
			settings.slots.push_back({ a_slots[i].on, a_slots[i].uid, a_slots[i].path });
			std::ofstream file(Settings::StatePath(i), std::ios::binary | std::ios::trunc);
			file.write(a_slots[i].state.data(), static_cast<std::streamsize>(a_slots[i].state.size()));
		}
		for (std::size_t i = a_slots.size(); std::filesystem::exists(Settings::StatePath(i), error); ++i) {
			std::filesystem::remove(Settings::StatePath(i), error);
		}
		settings.Save();
		logger::info("Saved {} slot(s).", a_slots.size());
	}

	using SetLevel_t = std::int32_t (*)(void*, HWND, std::uint32_t);
	SetLevel_t        originalSetLevel = nullptr;
	std::atomic<bool> keepShared{ false };

	std::int32_t KeepShared(void* a_device, HWND a_window, std::uint32_t a_flags)
	{
		constexpr std::uint32_t kExclusive = 0x1, kShared = 0x2, kMouse = 0x12;
		if (keepShared.load() && (a_flags & kExclusive)) {
			REX::W32::DIDEVCAPS caps{};
			caps.size = sizeof(caps);
			if (static_cast<REX::W32::IDirectInputDevice8A*>(a_device)->GetCapabilities(&caps) >= 0 && (caps.devType & 0xFF) == kMouse) {
				a_flags = (a_flags & ~kExclusive) | kShared;
				static std::atomic<int> told{ 0 };
				if (told.fetch_add(1) < 5) {
					logger::info("The game asked for the mouse in exclusive mode; kept it shared for the plugin windows.");
				}
			}
		}
		return originalSetLevel(a_device, a_window, a_flags);
	}

	void ShareMouse(bool a_share)
	{
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			return;
		}
		tasks->AddTask([a_share] {
			if (!a_share) {
				CursorGuard::ShowOnThisThread(false);
			}
			auto* input = RE::BSInputDeviceManager::GetSingleton();
			auto* mouse = input ? input->GetMouse() : nullptr;
			auto* main = RE::Main::GetSingleton();
			auto* device = mouse ? mouse->GetRuntimeData().dInputDevice : nullptr;
			if (!device || !main || !main->wnd) {
				logger::warn("The game's mouse could not be {}.", a_share ? "shared with plugin windows" : "taken back");
				return;
			}
			constexpr std::uint32_t kExclusive = 0x1, kShared = 0x2, kForeground = 0x4;
			if (!originalSetLevel) {
				auto** table = *reinterpret_cast<void***>(device);
				DWORD  old = 0;
				if (VirtualProtect(&table[13], sizeof(void*), PAGE_READWRITE, &old)) {
					originalSetLevel = reinterpret_cast<SetLevel_t>(table[13]);
					table[13] = reinterpret_cast<void*>(&KeepShared);
					VirtualProtect(&table[13], sizeof(void*), old, &old);
				}
			}
			keepShared = a_share;
			device->Unacquire();
			const auto result = device->SetCooperativeLevel(main->wnd, (a_share ? kShared : kExclusive) | kForeground);
			device->Acquire();
			if (result < 0) {
				logger::warn("The game's mouse mode could not be changed (0x{:08X}).", static_cast<std::uint32_t>(result));
			} else {
				logger::info("{}", a_share ? "Mouse shared with plugin windows." : "Mouse given back to the game.");
			}
		});
	}

	std::mutex        mouseLock;
	bool              mouseShared = false;
	std::atomic<bool> editorsOpen{ false };
	std::atomic<bool> choosing{ false };
	std::atomic<bool> pointerShared{ false };

	void UpdateMouse()
	{
		std::scoped_lock lock(mouseLock);
		const bool want = editorsOpen.load() || choosing.load();
		if (want != mouseShared) {
			mouseShared = want;
			if (want) {
				static const std::size_t redirected = [] {
					const auto count = CursorGuard::Install();
					logger::info("Pointer guard: {} SetCursor / ShowCursor import(s) redirected.", count);
					return count;
				}();
				(void)redirected;
			}
			CursorGuard::SetActive(want);
			pointerShared = want;
			ShareMouse(want);
		}
	}

	std::string Utf8(const wchar_t* a_text)
	{
		const int size = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
		std::string out(size > 0 ? static_cast<std::size_t>(size - 1) : 0, '\0');
		if (size > 1) {
			WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), size, nullptr, nullptr);
		}
		return out;
	}

	std::string ExtraFolder()
	{
		std::scoped_lock lock(settingsLock);
		return Settings::GetSingleton().extraFolder;
	}

	void SetExtraFolder(const std::string& a_folder)
	{
		{
			std::scoped_lock lock(settingsLock);
			auto&            settings = Settings::GetSingleton();
			settings.extraFolder = a_folder;
			settings.Save();
		}
		logger::info("Extra plugin folder: {}", a_folder.empty() ? "none" : a_folder);
		Rack::SetFolders(Folders(a_folder));
		Rack::Rescan();
	}

	void FolderDialog()
	{
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		CursorGuard::Protect(GetCurrentThreadId());
		UpdateMouse();
		HWND owner = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
		IFileOpenDialog* dialog = nullptr;
		std::string      chosen;
		bool             picked = false;
		if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
			FILEOPENDIALOGOPTIONS options{};
			dialog->GetOptions(&options);
			dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
			dialog->SetTitle(L"Folder with VST3 plugins");
			dialog->SetOkButtonLabel(L"Use this folder");
			const auto current = ExtraFolder();
			IShellItem* start = nullptr;
			if (!current.empty() && SUCCEEDED(SHCreateItemFromParsingName(std::filesystem::u8path(current).wstring().c_str(), nullptr, IID_PPV_ARGS(&start)))) {
				dialog->SetFolder(start);
				start->Release();
			}
			if (SUCCEEDED(dialog->Show(owner))) {
				IShellItem* item = nullptr;
				if (SUCCEEDED(dialog->GetResult(&item))) {
					PWSTR path = nullptr;
					if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
						chosen = Utf8(path);
						picked = true;
						CoTaskMemFree(path);
					}
					item->Release();
				}
			}
			dialog->Release();
		} else {
			logger::warn("The folder dialog could not be opened.");
		}
		if (owner) {
			DestroyWindow(owner);
		}
		CoUninitialize();
		CursorGuard::Unprotect(GetCurrentThreadId());
		choosing = false;
		UpdateMouse();
		if (picked) {
			SetExtraFolder(chosen);
		}
	}
}

namespace Host
{
	void Listen()
	{
		AudioChain::Listen("AudioPluginHost");
	}

	void Attach()
	{
		auto* platform = RE::BSAudioManager::QPlatformInstance();
		auto* master = platform ? static_cast<RE::BSXAudio2Audio*>(platform)->masteringVoice : nullptr;
		if (!master) {
			SetStatus("The game's master audio voice is not available.", true);
			return;
		}
		RE::XAUDIO2_VOICE_DETAILS details{};
		reinterpret_cast<RE::IXAudio2Voice*>(master)->GetVoiceDetails(&details);
		std::size_t position = 0, count = 0;
		const auto  result = AudioChain::Join(reinterpret_cast<RE::IXAudio2Voice*>(master), details.InputChannels, &effect, kChainOrder, &position, &count);
		if (result < 0) {
			SetStatus(std::format("The game refused the effect (0x{:08X}).", static_cast<std::uint32_t>(result)), true);
			return;
		}
		SetStatus(std::format("Attached: {} channels at {} Hz{}.", details.InputChannels, details.InputSampleRate, count > 1 ? std::format(", effect {} of {} on the master voice", position + 1, count) : ""), false);

		const auto&   opened = reinterpret_cast<const WAVEFORMATEX&>(static_cast<RE::BSXAudio2Audio*>(platform)->outputFormat);
		std::uint32_t device = opened.nChannels >= 1 && opened.nChannels <= 16 ? opened.nChannels : 0;
		if (device) {
			logger::info("The game's playback device: {} channel(s) at {} Hz.", device, opened.nSamplesPerSec);
		} else {
			device = DeviceChannels();
			logger::info("Windows default playback device: {} channel(s).", device);
		}
		if (details.InputChannels == 6 && device == 2) {
			stereoAvailable = true;
			stereoNote.clear();
		} else if (details.InputChannels != 6) {
			stereoNote = std::format("Not needed: the game's mix has {} channels, not 6.", details.InputChannels);
		} else {
			stereoNote = device ? std::format("Not available: your Windows playback device has {} channels, not 2.", device) : "Not available: the Windows playback device could not be read.";
		}

		const auto&  settings = Settings::GetSingleton();
		Rack::Config config;
		config.stereo = settings.stereoMaster && stereoAvailable.load();
		config.folders = Folders(ExtraFolder());
		config.rate = details.InputSampleRate;
		config.channels = details.InputChannels;
		config.enabled = settings.enabled;
		config.editorsOnTop = settings.editorsOnTop;
		for (std::size_t i = 0; i < settings.slots.size(); ++i) {
			Rack::Saved   slot{ settings.slots[i].on, settings.slots[i].uid, settings.slots[i].path, {} };
			std::ifstream file(Settings::StatePath(i), std::ios::binary);
			slot.state.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			config.slots.push_back(std::move(slot));
		}
		config.log = [](bool a_warning, const std::string& a_text) {
			if (a_warning) {
				logger::warn("{}", a_text);
			} else {
				logger::info("{}", a_text);
			}
		};
		config.onEditors = [](bool a_open) {
			CursorGuard::Protect(GetCurrentThreadId());
			editorsOpen = a_open;
			UpdateMouse();
		};
		config.onIdle = [] {
			if (const int change = CursorGuard::Tick()) {
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([show = change > 0] { CursorGuard::ShowOnThisThread(show); });
				}
			}
		};
		config.onSave = [](const std::vector<Rack::Saved>& a_slots, bool a_states) {
			if (a_states) {
				SaveSlots(a_slots);
			}
		};
		Rack::Start(std::move(config));
	}

	std::string LoadText() { return effect.LoadText(); }

	std::string Status()
	{
		std::scoped_lock lock(statusLock);
		return status;
	}

	void SaveSettings()
	{
		std::scoped_lock lock(settingsLock);
		Settings::GetSingleton().Save();
	}

	void ChooseFolder()
	{
		if (!choosing.exchange(true)) {
			std::thread(FolderDialog).detach();
		}
	}

	void ClearFolder()
	{
		SetExtraFolder({});
	}

	bool Choosing() { return choosing.load(); }

	bool PointerOnPlugin() { return pointerShared.load(std::memory_order_relaxed) && CursorGuard::OverProtected(); }

	std::string Folder() { return ExtraFolder(); }

	bool        StereoAvailable() { return stereoAvailable.load(); }
	std::string StereoNote() { return stereoNote; }
}
