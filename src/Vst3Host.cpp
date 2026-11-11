#include "Vst3Host.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>

#include <bit>
#include <format>

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace
{
	class Host final : public HostApplication
	{
	public:
		tresult PLUGIN_API getName(String128 a_name) override { return VST3::StringConvert::convert("Audio Plugin Host", a_name) ? kResultTrue : kInternalError; }
	};

	FUnknown* HostContext()
	{
		static IPtr<Host> host = [] {
			auto made = owned(new Host);
			PluginContextFactory::instance().setPluginContext(made);
			return made;
		}();
		return host;
	}

	std::string Utf8(const std::filesystem::path& a_path)
	{
		const auto text = a_path.u8string();
		return std::string(text.begin(), text.end());
	}

	std::wstring Wide(const std::string& a_text)
	{
		const int    size = MultiByteToWideChar(CP_UTF8, 0, a_text.c_str(), -1, nullptr, 0);
		std::wstring out(size > 0 ? size - 1 : 0, L'\0');
		if (size > 1) {
			MultiByteToWideChar(CP_UTF8, 0, a_text.c_str(), -1, out.data(), size);
		}
		return out;
	}

	HINSTANCE ThisModule()
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ThisModule), &module);
		return module;
	}

	bool SafeProcess(IAudioProcessor* a_processor, ProcessData* a_data)
	{
		__try {
			a_processor->process(*a_data);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	struct IgnoreEdits final : U::ImplementsNonDestroyable<U::Directly<IComponentHandler>>
	{
		tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
		tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
		tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
		tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
	};
	IgnoreEdits ignoreEdits;

	constexpr wchar_t kWindowClass[] = L"AudioPluginHostEditor";

	void Trace(const char* a_step)
	{
		static const bool on = GetEnvironmentVariableA("APH_TRACE", nullptr, 0) > 0;
		if (on) {
			OutputDebugStringA(a_step);
			std::fprintf(stderr, "      trace: %s\n", a_step);
		}
	}
}

namespace Vst
{
	std::vector<PluginInfo> Scan(const std::vector<std::filesystem::path>& a_folders, std::vector<std::string>* a_problems)
	{
		HostContext();
		std::vector<PluginInfo> found;
		for (const auto& folder : a_folders) {
			std::error_code error;
			if (!std::filesystem::is_directory(folder, error)) {
				continue;
			}
			for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, error);
				 it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
				if (error) {
					break;
				}
				const auto& path = it->path();
				if (_wcsicmp(path.extension().c_str(), L".vst3") != 0) {
					continue;
				}
				if (it->is_directory(error)) {
					it.disable_recursion_pending();
				}
				std::string problem;
				const auto  module = VST3::Hosting::Module::create(Utf8(path), problem);
				if (!module) {
					if (a_problems) {
						a_problems->push_back(Utf8(path.filename()) + ": " + problem);
					}
					continue;
				}
				for (const auto& info : module->getFactory().classInfos()) {
					if (info.category() == kVstAudioEffectClass && info.subCategoriesString().find("Fx") != std::string::npos) {
						found.push_back({ Utf8(path), info.ID().toString(), info.name(), info.vendor() });
					}
				}
			}
		}
		std::sort(found.begin(), found.end(), [](const PluginInfo& a, const PluginInfo& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
		return found;
	}

	struct Instance
	{
		IPtr<IComponent>        component;
		IPtr<IAudioProcessor>   processor;
		IPtr<IEditController>   controller;
		bool                    separateController{ false };
		IPtr<IConnectionPoint>  componentPoint;
		IPtr<IConnectionPoint>  controllerPoint;
		HostProcessData         data;
		ParameterChanges        changes{ 512 };
		ParameterChangeTransfer transfer{ 4096 };
		ProcessContext          context{};
		std::vector<int>        inputs;
		std::vector<int>        outputs;
		bool                    active{ false };
		bool                    processing{ false };
	};

	struct Slot::Impl
	{
		struct Handler final : U::ImplementsNonDestroyable<U::Directly<IComponentHandler>>
		{
			Impl* owner{ nullptr };
			tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
			tresult PLUGIN_API performEdit(ParamID a_id, ParamValue a_value) override
			{
				for (std::size_t i = 0; i < owner->instances.size(); ++i) {
					auto& instance = *owner->instances[i];
					instance.transfer.addChange(a_id, a_value, 0);
					if (i > 0 && instance.separateController) {
						instance.controller->setParamNormalized(a_id, a_value);
					}
				}
				return kResultOk;
			}
			tresult PLUGIN_API endEdit(ParamID) override
			{
				owner->Touch();
				return kResultOk;
			}
			tresult PLUGIN_API restartComponent(int32 a_flags) override
			{
				if ((a_flags & kLatencyChanged) && !owner->instances.empty()) {
					owner->latency = static_cast<std::int32_t>(owner->instances[0]->processor->getLatencySamples());
				}
				owner->Touch();
				return kResultOk;
			}
		};

		struct Frame final : U::ImplementsNonDestroyable<U::Directly<IPlugFrame>>
		{
			Impl* owner{ nullptr };
			tresult PLUGIN_API resizeView(IPlugView* a_view, ViewRect* a_size) override
			{
				if (!a_view || !a_size || !owner->window) {
					return kInvalidArgument;
				}
				owner->ResizeClient(a_size->getWidth(), a_size->getHeight());
				a_view->onSize(a_size);
				return kResultTrue;
			}
		};

		PluginInfo                             info;
		VST3::Hosting::Module::Ptr             module;
		double                                 rate{ 48000.0 };
		std::uint32_t                          channels{ 2 };
		std::string                            layout;
		std::vector<std::unique_ptr<Instance>> instances;
		Handler                                handler;
		Frame                                  frame;
		IPtr<IPlugView>                        view;
		HWND                                   window{ nullptr };
		bool                                   resizable{ false };
		std::atomic<std::int32_t>              latency{ 0 };
		std::atomic<bool>                      crashed{ false };
		bool                                   dirty{ false };
		ULONGLONG                              lastEdit{ 0 };
		float                                  mix{ 1.0f };
		bool                                   direct{ false };
		bool                                   asleep{ false };
		std::int64_t                           warmup{ 0 };
		TSamples                               time{ 0 };
		std::vector<float>                     dry;
		std::vector<float>                     sum;

		void Touch()
		{
			dirty = true;
			lastEdit = GetTickCount64();
		}

		void ResizeClient(int a_width, int a_height)
		{
			RECT       rect{ 0, 0, a_width, a_height };
			const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
			const auto ex = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
			AdjustWindowRectEx(&rect, style, FALSE, ex);
			SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}

		void CopyState()
		{
			if (instances.size() < 2) {
				return;
			}
			MemoryStream state;
			if (instances[0]->component->getState(&state) != kResultOk) {
				return;
			}
			for (std::size_t i = 1; i < instances.size(); ++i) {
				state.seek(0, IBStream::kIBSeekSet, nullptr);
				instances[i]->component->setState(&state);
				if (instances[i]->separateController) {
					state.seek(0, IBStream::kIBSeekSet, nullptr);
					instances[i]->controller->setComponentState(&state);
				}
			}
		}

		void Stop(Instance& a_instance)
		{
			if (a_instance.processing) {
				a_instance.processor->setProcessing(false);
				a_instance.processing = false;
			}
			if (a_instance.active) {
				a_instance.component->setActive(false);
				a_instance.active = false;
			}
			if (a_instance.componentPoint && a_instance.controllerPoint) {
				a_instance.componentPoint->disconnect(a_instance.controllerPoint);
				a_instance.controllerPoint->disconnect(a_instance.componentPoint);
			}
			a_instance.componentPoint = nullptr;
			a_instance.controllerPoint = nullptr;
			if (a_instance.controller) {
				a_instance.controller->setComponentHandler(nullptr);
				if (a_instance.separateController) {
					a_instance.controller->terminate();
				}
			}
			a_instance.controller = nullptr;
			a_instance.processor = nullptr;
			if (a_instance.component) {
				a_instance.component->terminate();
			}
			a_instance.component = nullptr;
			a_instance.data.unprepare();
		}

		std::unique_ptr<Instance> Make(const VST3::UID& a_uid, bool a_main, std::string& a_error)
		{
			Trace("make");
			auto        instance = std::make_unique<Instance>();
			const auto& factory = module->getFactory();
			instance->component = factory.createInstance<IComponent>(a_uid);
			if (!instance->component || instance->component->initialize(HostContext()) != kResultOk) {
				a_error = "The plugin could not be created.";
				return nullptr;
			}
			Trace("component initialized");
			FUnknownPtr<IAudioProcessor> processor(instance->component);
			if (!processor) {
				a_error = "The plugin does not process audio.";
				instance->component->terminate();
				return nullptr;
			}
			instance->processor = processor;
			TUID controllerId{};
			if (instance->component->getControllerClassId(controllerId) == kResultTrue) {
				auto controller = factory.createInstance<IEditController>(VST3::UID::fromTUID(controllerId));
				if (controller && controller->initialize(HostContext()) == kResultOk) {
					instance->controller = controller;
					instance->separateController = true;
				}
			}
			if (!instance->controller) {
				FUnknownPtr<IEditController> same(instance->component);
				if (same) {
					instance->controller = same;
				}
			}
			Trace("controller made");
			if (instance->controller) {
				if (instance->separateController) {
					FUnknownPtr<IConnectionPoint> componentPoint(instance->component), controllerPoint(instance->controller);
					if (componentPoint && controllerPoint) {
						componentPoint->connect(controllerPoint);
						controllerPoint->connect(componentPoint);
						instance->componentPoint = componentPoint;
						instance->controllerPoint = controllerPoint;
					}
					MemoryStream state;
					if (instance->component->getState(&state) == kResultOk) {
						state.seek(0, IBStream::kIBSeekSet, nullptr);
						instance->controller->setComponentState(&state);
					}
				}
				instance->controller->setComponentHandler(a_main ? static_cast<IComponentHandler*>(&handler) : static_cast<IComponentHandler*>(&ignoreEdits));
			}
			return instance;
		}

		static bool Arrange(Instance& a_instance, SpeakerArrangement a_arrangement)
		{
			Trace("arrange");
			const int32 inputs = a_instance.component->getBusCount(kAudio, kInput);
			const int32 outputs = a_instance.component->getBusCount(kAudio, kOutput);
			if (inputs < 1 || outputs < 1) {
				return false;
			}
			std::vector<SpeakerArrangement> in(inputs), out(outputs);
			for (int32 i = 0; i < inputs; ++i) {
				a_instance.processor->getBusArrangement(kInput, i, in[i]);
			}
			for (int32 i = 0; i < outputs; ++i) {
				a_instance.processor->getBusArrangement(kOutput, i, out[i]);
			}
			in[0] = out[0] = a_arrangement;
			a_instance.processor->setBusArrangements(in.data(), inputs, out.data(), outputs);
			SpeakerArrangement gotIn = 0, gotOut = 0;
			a_instance.processor->getBusArrangement(kInput, 0, gotIn);
			a_instance.processor->getBusArrangement(kOutput, 0, gotOut);
			return gotIn == a_arrangement && gotOut == a_arrangement;
		}

		bool Start(Instance& a_instance, std::string& a_error)
		{
			const int32 inputs = a_instance.component->getBusCount(kAudio, kInput);
			const int32 outputs = a_instance.component->getBusCount(kAudio, kOutput);
			for (int32 i = 0; i < inputs; ++i) {
				a_instance.component->activateBus(kAudio, kInput, i, i == 0);
			}
			for (int32 i = 0; i < outputs; ++i) {
				a_instance.component->activateBus(kAudio, kOutput, i, i == 0);
			}
			for (int32 i = 0; i < a_instance.component->getBusCount(kEvent, kInput); ++i) {
				a_instance.component->activateBus(kEvent, kInput, i, false);
			}
			for (int32 i = 0; i < a_instance.component->getBusCount(kEvent, kOutput); ++i) {
				a_instance.component->activateBus(kEvent, kOutput, i, false);
			}
			Trace("buses activated");
			if (a_instance.processor->canProcessSampleSize(kSample32) != kResultTrue) {
				a_error = "The plugin cannot process 32-bit audio.";
				return false;
			}
			ProcessSetup setup{ kRealtime, kSample32, kMaxBlock, rate };
			if (a_instance.processor->setupProcessing(setup) != kResultOk) {
				a_error = "The plugin refused the sample rate or block size.";
				return false;
			}
			Trace("setup done");
			if (a_instance.component->setActive(true) != kResultOk) {
				a_error = "The plugin could not be switched on.";
				return false;
			}
			a_instance.active = true;
			a_instance.data.prepare(*a_instance.component, kMaxBlock, kSample32);
			a_instance.data.inputParameterChanges = &a_instance.changes;
			a_instance.data.processContext = &a_instance.context;
			a_instance.context.sampleRate = rate;
			a_instance.context.tempo = 120.0;
			a_instance.context.timeSigNumerator = 4;
			a_instance.context.timeSigDenominator = 4;
			a_instance.context.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kContTimeValid;
			Trace("active");
			a_instance.processor->setProcessing(true);
			a_instance.processing = true;
			Trace("processing");
			return true;
		}

		void ProcessBlock(float* a_samples, std::uint32_t a_frames, std::uint32_t a_stride, std::uint32_t a_channels, bool a_wanted)
		{
			const bool warming = warmup > 0;
			const bool fading = warming || (a_wanted ? mix < 1.0f : true);
			if (fading) {
				std::memcpy(dry.data(), a_samples, static_cast<std::size_t>(a_frames) * a_stride * sizeof(float));
			}
			if (!direct) {
				std::fill(sum.begin(), sum.begin() + static_cast<std::size_t>(a_frames) * a_channels, 0.0f);
			}
			std::uint32_t used[kMaxChannels]{};
			for (auto& owned : instances) {
				auto& instance = *owned;
				auto& in = instance.data.inputs[0];
				in.silenceFlags = 0;
				for (std::size_t j = 0; j < instance.inputs.size() && j < static_cast<std::size_t>(in.numChannels); ++j) {
					float*       to = in.channelBuffers32[j];
					const int    from = instance.inputs[j];
					std::uint32_t any = 0;
					for (std::uint32_t i = 0; i < a_frames; ++i) {
						const float value = a_samples[static_cast<std::size_t>(i) * a_stride + from];
						to[i] = value;
						any |= std::bit_cast<std::uint32_t>(value) << 1;
					}
					if (!any) {
						in.silenceFlags |= 1ull << j;
					}
				}
				instance.transfer.transferChangesTo(instance.changes);
				instance.context.projectTimeSamples = time;
				instance.context.continousTimeSamples = time;
				instance.data.numSamples = static_cast<int32>(a_frames);
				if (!SafeProcess(instance.processor, &instance.data)) {
					crashed = true;
					if (fading) {
						std::memcpy(a_samples, dry.data(), static_cast<std::size_t>(a_frames) * a_stride * sizeof(float));
					}
					return;
				}
				instance.changes.clearQueue();
				const auto& out = instance.data.outputs[0];
				for (std::size_t j = 0; j < instance.outputs.size() && j < static_cast<std::size_t>(out.numChannels); ++j) {
					const int  to = instance.outputs[j];
					const bool silent = (out.silenceFlags & (1ull << j)) != 0;
					if (direct) {
						float* target = a_samples + to;
						if (silent) {
							for (std::uint32_t i = 0; i < a_frames; ++i) {
								target[static_cast<std::size_t>(i) * a_stride] = 0.0f;
							}
						} else {
							const float* from = out.channelBuffers32[j];
							for (std::uint32_t i = 0; i < a_frames; ++i) {
								target[static_cast<std::size_t>(i) * a_stride] = from[i];
							}
						}
						continue;
					}
					++used[to];
					if (silent) {
						continue;
					}
					const float* from = out.channelBuffers32[j];
					float*       target = sum.data() + static_cast<std::size_t>(to) * a_frames;
					for (std::uint32_t i = 0; i < a_frames; ++i) {
						target[i] += from[i];
					}
				}
			}
			time += a_frames;
			warmup -= a_frames;
			for (std::uint32_t channel = 0; channel < a_channels; ++channel) {
				if (!used[channel]) {
					continue;
				}
				const float  scale = 1.0f / static_cast<float>(used[channel]);
				const float* from = sum.data() + static_cast<std::size_t>(channel) * a_frames;
				for (std::uint32_t i = 0; i < a_frames; ++i) {
					a_samples[static_cast<std::size_t>(i) * a_stride + channel] = from[i] * scale;
				}
			}
			if (fading) {
				const float step = 1.0f / static_cast<float>(0.01 * rate);
				const float goal = a_wanted && !warming ? 1.0f : 0.0f;
				for (std::uint32_t i = 0; i < a_frames; ++i) {
					mix = goal > mix ? (std::min)(goal, mix + step) : (std::max)(goal, mix - step);
					for (std::uint32_t channel = 0; channel < a_channels; ++channel) {
						const std::size_t at = static_cast<std::size_t>(i) * a_stride + channel;
						a_samples[at] = dry[at] + (a_samples[at] - dry[at]) * mix;
					}
				}
			}
		}

		static LRESULT CALLBACK EditorProc(HWND a_window, UINT a_message, WPARAM a_w, LPARAM a_l)
		{
			auto* owner = reinterpret_cast<Impl*>(GetWindowLongPtrW(a_window, GWLP_USERDATA));
			switch (a_message) {
			case WM_CLOSE:
				if (owner) {
					owner->CloseEditor();
					return 0;
				}
				break;
			case WM_SIZE:
				if (owner && owner->view && owner->resizable && a_w != SIZE_MINIMIZED) {
					ViewRect size{ 0, 0, LOWORD(a_l), HIWORD(a_l) };
					owner->view->onSize(&size);
				}
				break;
			default:
				break;
			}
			return DefWindowProcW(a_window, a_message, a_w, a_l);
		}

		void CloseEditor()
		{
			if (view) {
				view->setFrame(nullptr);
				view->removed();
				view = nullptr;
			}
			if (window) {
				SetWindowLongPtrW(window, GWLP_USERDATA, 0);
				DestroyWindow(window);
				window = nullptr;
			}
			Touch();
		}

		~Impl()
		{
			CloseEditor();
			for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
				Stop(**it);
			}
			instances.clear();
			module.reset();
		}
	};

	std::unique_ptr<Slot> Slot::Create(const PluginInfo& a_info, double a_rate, std::uint32_t a_channels, std::string& a_error)
	{
		HostContext();
		std::unique_ptr<Slot> slot(new Slot);
		slot->impl = std::make_unique<Impl>();
		auto& m = *slot->impl;
		m.handler.owner = &m;
		m.frame.owner = &m;
		m.info = a_info;
		m.rate = a_rate;
		m.channels = (std::min)(a_channels, kMaxChannels);
		m.dry.resize(static_cast<std::size_t>(kMaxBlock) * kMaxChannels);
		m.sum.resize(static_cast<std::size_t>(kMaxBlock) * kMaxChannels);
		m.module = VST3::Hosting::Module::create(a_info.path, a_error);
		if (!m.module) {
			return nullptr;
		}
		const auto uid = VST3::UID::fromString(a_info.uid);
		if (!uid) {
			a_error = "The saved class ID is not valid.";
			return nullptr;
		}

		struct Group
		{
			SpeakerArrangement arrangement;
			std::vector<int>   inputs;
			std::vector<int>   outputs;
		};
		struct Plan
		{
			const char*        name;
			std::vector<Group> groups;
		};
		std::vector<Plan> plans;
		const int         n = static_cast<int>(m.channels);
		if (n == 6) {
			plans.push_back({ "5.1", { { SpeakerArr::k51, { 0, 1, 2, 3, 4, 5 }, { 0, 1, 2, 3, 4, 5 } } } });
		}
		{
			Plan stereo{ "stereo", {} };
			if (n >= 6) {
				stereo.groups = { { SpeakerArr::kStereo, { 0, 1 }, { 0, 1 } }, { SpeakerArr::kStereo, { 2, 2 }, { 2, 2 } }, { SpeakerArr::kStereo, { 4, 5 }, { 4, 5 } } };
				if (n >= 8) {
					stereo.groups.push_back({ SpeakerArr::kStereo, { 6, 7 }, { 6, 7 } });
				}
			} else if (n >= 2) {
				for (int c = 0; c + 1 < n; c += 2) {
					stereo.groups.push_back({ SpeakerArr::kStereo, { c, c + 1 }, { c, c + 1 } });
				}
			}
			if (!stereo.groups.empty()) {
				plans.push_back(stereo);
			}
			Plan mono{ "mono", {} };
			for (int c = 0; c < n; ++c) {
				if (!(n >= 6 && c == 3)) {
					mono.groups.push_back({ SpeakerArr::kMono, { c }, { c } });
				}
			}
			plans.push_back(mono);
		}

		auto first = m.Make(*uid, true, a_error);
		if (!first) {
			return nullptr;
		}
		const Plan* chosen = nullptr;
		for (const auto& plan : plans) {
			if (Impl::Arrange(*first, plan.groups[0].arrangement)) {
				chosen = &plan;
				break;
			}
		}
		if (!chosen) {
			m.Stop(*first);
			a_error = "The plugin takes none of the layouts the game can use (5.1, stereo or mono).";
			return nullptr;
		}
		first->inputs = chosen->groups[0].inputs;
		first->outputs = chosen->groups[0].outputs;
		m.instances.push_back(std::move(first));
		for (std::size_t g = 1; g < chosen->groups.size(); ++g) {
			auto copy = m.Make(*uid, false, a_error);
			if (!copy || !Impl::Arrange(*copy, chosen->groups[g].arrangement)) {
				if (copy) {
					m.Stop(*copy);
				}
				a_error = a_error.empty() ? "A linked copy of the plugin refused the layout." : a_error;
				return nullptr;
			}
			copy->inputs = chosen->groups[g].inputs;
			copy->outputs = chosen->groups[g].outputs;
			m.instances.push_back(std::move(copy));
		}
		for (auto& instance : m.instances) {
			if (!m.Start(*instance, a_error)) {
				return nullptr;
			}
		}
		m.latency = static_cast<std::int32_t>(m.instances[0]->processor->getLatencySamples());
		if (m.instances.size() == 1) {
			std::uint32_t seen = 0;
			m.direct = true;
			for (const int channel : m.instances[0]->outputs) {
				m.direct = m.direct && channel >= 0 && channel < static_cast<int>(a_channels) && !(seen & (1u << (channel & 31)));
				seen |= 1u << (channel & 31);
			}
		}
		m.layout = chosen->groups.size() == 1 ? chosen->name : std::format("{} x {}", chosen->groups.size(), chosen->name);
		m.CopyState();
		return slot;
	}

	Slot::~Slot() = default;

	const PluginInfo& Slot::Info() const { return impl->info; }
	std::string       Slot::Layout() const { return impl->layout; }
	std::int32_t      Slot::Latency() const { return impl->latency.load(); }
	bool              Slot::EditorOpen() const { return impl->window != nullptr; }
	bool              Slot::Crashed() const { return impl->crashed.load(); }

	bool Slot::HasEditor() const
	{
		return impl->instances[0]->controller != nullptr;
	}

	bool Slot::OpenEditor(bool a_onTop)
	{
		auto& m = *impl;
		if (m.window) {
			ShowWindow(m.window, SW_SHOWNORMAL);
			SetForegroundWindow(m.window);
			return true;
		}
		auto& controller = m.instances[0]->controller;
		if (!controller) {
			return false;
		}
		IPtr<IPlugView> view = owned(controller->createView(ViewType::kEditor));
		if (!view || view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) {
			return false;
		}
		static const bool registered = [] {
			WNDCLASSEXW type{ sizeof(type) };
			type.style = CS_DBLCLKS;
			type.lpfnWndProc = &Impl::EditorProc;
			type.hInstance = ThisModule();
			type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			type.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
			type.lpszClassName = kWindowClass;
			return RegisterClassExW(&type) != 0;
		}();
		if (!registered) {
			return false;
		}
		ViewRect size{ 0, 0, 800, 500 };
		view->getSize(&size);
		m.resizable = view->canResize() == kResultTrue;
		const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | (m.resizable ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0);
		const DWORD ex = a_onTop ? WS_EX_TOPMOST : 0;
		RECT        rect{ 0, 0, size.getWidth(), size.getHeight() };
		AdjustWindowRectEx(&rect, style, FALSE, ex);
		const auto title = Wide(m.info.name);
		m.window = CreateWindowExW(ex, kWindowClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, ThisModule(), nullptr);
		if (!m.window) {
			return false;
		}
		SetWindowLongPtrW(m.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&m));
		m.view = view;
		view->setFrame(&m.frame);
		if (FUnknownPtr<IPlugViewContentScaleSupport> scale(view); scale) {
			using GetDpi_t = UINT(WINAPI*)(HWND);
			static const auto getDpi = reinterpret_cast<GetDpi_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
			const UINT        dpi = getDpi ? getDpi(m.window) : 96;
			scale->setContentScaleFactor(static_cast<IPlugViewContentScaleSupport::ScaleFactor>(dpi ? dpi : 96) / 96.0f);
		}
		if (view->attached(m.window, kPlatformTypeHWND) != kResultOk) {
			m.CloseEditor();
			return false;
		}
		if (view->getSize(&size) == kResultOk) {
			m.ResizeClient(size.getWidth(), size.getHeight());
		}
		ShowWindow(m.window, SW_SHOWNORMAL);
		SetForegroundWindow(m.window);
		return true;
	}

	void Slot::CloseEditor()
	{
		impl->CloseEditor();
	}

	void Slot::SetEditorOnTop(bool a_onTop)
	{
		if (impl->window) {
			SetWindowPos(impl->window, a_onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
	}

	void Slot::Idle()
	{
		auto& m = *impl;
		if (m.dirty && GetTickCount64() - m.lastEdit > 250) {
			m.dirty = false;
			m.CopyState();
		}
	}

	std::vector<char> Slot::SaveState() const
	{
		auto&        main = *impl->instances[0];
		MemoryStream component, controller;
		main.component->getState(&component);
		if (main.separateController) {
			main.controller->getState(&controller);
		}
		std::vector<char> out;
		const auto        put = [&](const MemoryStream& a_stream) {
            const auto size = static_cast<std::uint32_t>(a_stream.getSize());
            out.insert(out.end(), reinterpret_cast<const char*>(&size), reinterpret_cast<const char*>(&size) + 4);
            out.insert(out.end(), a_stream.getData(), a_stream.getData() + size);
		};
		out.insert(out.end(), { 'A', 'P', 'H', '1' });
		put(component);
		put(controller);
		return out;
	}

	bool Slot::LoadState(const std::vector<char>& a_state)
	{
		if (a_state.size() < 12 || std::memcmp(a_state.data(), "APH1", 4) != 0) {
			return false;
		}
		std::size_t at = 4;
		const auto  take = [&](std::vector<char>& a_out) {
            if (at + 4 > a_state.size()) {
                return false;
            }
            std::uint32_t size = 0;
            std::memcpy(&size, a_state.data() + at, 4);
            at += 4;
            if (at + size > a_state.size()) {
                return false;
            }
            a_out.assign(a_state.begin() + at, a_state.begin() + at + size);
            at += size;
            return true;
		};
		std::vector<char> component, controller;
		if (!take(component) || !take(controller)) {
			return false;
		}
		for (auto& instance : impl->instances) {
			MemoryStream state(component.data(), static_cast<TSize>(component.size()));
			instance->component->setState(&state);
			if (instance->separateController) {
				state.seek(0, IBStream::kIBSeekSet, nullptr);
				instance->controller->setComponentState(&state);
				if (!controller.empty()) {
					MemoryStream extra(controller.data(), static_cast<TSize>(controller.size()));
					instance->controller->setState(&extra);
				}
			}
		}
		impl->latency = static_cast<std::int32_t>(impl->instances[0]->processor->getLatencySamples());
		return true;
	}

	bool Slot::Busy() const
	{
		const auto& m = *impl;
		return !m.crashed.load(std::memory_order_relaxed) && (enabled.load(std::memory_order_relaxed) || m.mix > 0.0f);
	}

	void Slot::Process(float* a_samples, std::uint32_t a_frames, std::uint32_t a_channels)
	{
		auto& m = *impl;
		if (m.crashed.load(std::memory_order_relaxed)) {
			return;
		}
		const bool wanted = enabled.load(std::memory_order_relaxed);
		if (!wanted && m.mix <= 0.0f) {
			m.asleep = true;
			return;
		}
		if (m.asleep) {
			m.asleep = false;
			m.warmup = static_cast<std::int64_t>(m.latency.load(std::memory_order_relaxed)) + kMaxBlock;
		}
		const std::uint32_t channels = (std::min)(a_channels, m.channels);
		for (std::uint32_t done = 0; done < a_frames;) {
			const std::uint32_t frames = (std::min)(a_frames - done, static_cast<std::uint32_t>(kMaxBlock));
			m.ProcessBlock(a_samples + static_cast<std::size_t>(done) * a_channels, frames, a_channels, channels, wanted);
			done += frames;
		}
		if (!wanted && m.mix <= 0.0f) {
			m.asleep = true;
		}
	}
}
