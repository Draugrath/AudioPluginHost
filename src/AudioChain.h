#pragma once

#include <RE/I/IXAudio2Voice.h>
#include <RE/X/XAPO.h>
#include <RE/X/XAudio2.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace AudioChain
{
	inline constexpr std::uint32_t kAnnounce = 0x41434831;
	inline constexpr const char*   kPeers[]{ "AudioEqualizer", "AudioPluginHost" };

	struct Announcement
	{
		std::uint32_t size{ sizeof(Announcement) };
		RE::IXAPO*    effect{ nullptr };
		std::int32_t  order{ 0 };
	};

	inline std::vector<Announcement>& Known()
	{
		static std::vector<Announcement> known;
		return known;
	}

	inline void Note(const Announcement& a_announcement)
	{
		auto& known = Known();
		const auto same = std::find_if(known.begin(), known.end(), [&](const Announcement& a) { return a.effect == a_announcement.effect; });
		if (same != known.end()) {
			*same = a_announcement;
		} else {
			known.push_back(a_announcement);
		}
	}

	inline void OnPeer(SKSE::MessagingInterface::Message* a_message)
	{
		if (a_message && a_message->type == kAnnounce && a_message->data && a_message->dataLen >= sizeof(Announcement)) {
			const auto& announcement = *static_cast<const Announcement*>(a_message->data);
			if (announcement.size >= sizeof(Announcement) && announcement.effect) {
				Note(announcement);
			}
		}
	}

	inline void Listen(std::string_view a_self)
	{
		auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging) {
			return;
		}
		for (const char* peer : kPeers) {
			if (a_self != peer) {
				messaging->RegisterListener(peer, OnPeer);
			}
		}
	}

	inline std::int32_t Join(RE::IXAudio2Voice* a_voice, std::uint32_t a_channels, RE::IXAPO* a_effect, std::int32_t a_order, std::size_t* a_position = nullptr, std::size_t* a_count = nullptr)
	{
		Announcement self;
		self.effect = a_effect;
		self.order = a_order;
		Note(self);
		if (auto* messaging = SKSE::GetMessagingInterface()) {
			messaging->Dispatch(kAnnounce, &self, sizeof(self), nullptr);
		}
		auto chain = Known();
		std::stable_sort(chain.begin(), chain.end(), [](const Announcement& a, const Announcement& b) { return a.order < b.order; });
		std::vector<RE::XAUDIO2_EFFECT_DESCRIPTOR> descriptors;
		for (std::size_t i = 0; i < chain.size(); ++i) {
			descriptors.push_back({ chain[i].effect, 1, a_channels });
			if (chain[i].effect == a_effect && a_position) {
				*a_position = i;
			}
		}
		if (a_count) {
			*a_count = descriptors.size();
		}
		RE::XAUDIO2_EFFECT_CHAIN effects{ static_cast<std::uint32_t>(descriptors.size()), descriptors.data() };
		return a_voice->SetEffectChain(&effects);
	}
}
